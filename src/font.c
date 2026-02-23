#include "font.h"
#include "storage_paths.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "esp_log.h"

static const char *TAG = "font";

#define FONT_GLYPH_COUNT (FONT_LAST_CHAR - FONT_FIRST_CHAR + 1)
#define FONT_DATA_BYTES  (FONT_GLYPH_COUNT * FONT_WIDTH)

static uint8_t font_data[FONT_GLYPH_COUNT][FONT_WIDTH];
static bool font_loaded = false;

static void load_fallback_font(void)
{
    static const uint8_t question_mark[FONT_WIDTH] = {0x02, 0x01, 0x51, 0x09, 0x06};

    memset(font_data, 0, sizeof(font_data));
    for (int i = 0; i < FONT_GLYPH_COUNT; i++) {
        memcpy(font_data[i], question_mark, FONT_WIDTH);
    }
}

esp_err_t font_init(void)
{
    FILE *fp = fopen(LITTLEFS_FONT_PATH, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "Font file missing (%s); using fallback glyphs", LITTLEFS_FONT_PATH);
        load_fallback_font();
        font_loaded = true;
        return ESP_FAIL;
    }

    size_t read = fread(font_data, 1, FONT_DATA_BYTES, fp);
    fclose(fp);

    if (read != FONT_DATA_BYTES) {
        ESP_LOGW(TAG, "Font file size mismatch (%u bytes); using fallback glyphs", (unsigned)read);
        load_fallback_font();
        font_loaded = true;
        return ESP_FAIL;
    }

    font_loaded = true;
    ESP_LOGI(TAG, "Loaded font data from LittleFS (%u bytes)", FONT_DATA_BYTES);
    return ESP_OK;
}

const uint8_t *font_get_glyph(char c)
{
    if (!font_loaded) {
        load_fallback_font();
        font_loaded = true;
    }

    if (c < FONT_FIRST_CHAR || c > FONT_LAST_CHAR) {
        return NULL;
    }
    return font_data[c - FONT_FIRST_CHAR];
}

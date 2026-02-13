#include "text_scroller.h"
#include "led_panel.h"
#include "font.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "scroller";

static char current_text[SCROLLER_MAX_TEXT_LEN + 1] = "";
static uint8_t color_r = 255, color_g = 0, color_b = 0;
static uint8_t scroll_speed = 5; // 1-10
static int scroll_x = 0;
static SemaphoreHandle_t scroller_mutex = NULL;

static void render_frame(void)
{
    led_panel_clear();

    int text_len = strlen(current_text);
    if (text_len == 0) {
        led_panel_refresh();
        return;
    }

    int char_width = FONT_WIDTH + 1; // 5px glyph + 1px gap
    int total_width = text_len * char_width + led_panel_get_cols();

    for (int col = 0; col < led_panel_get_cols(); col++) {
        int virtual_col = (scroll_x + col) % total_width;
        int char_index = virtual_col / char_width;
        int col_in_char = virtual_col % char_width;

        if (char_index >= text_len || col_in_char >= FONT_WIDTH) {
            continue;
        }

        const uint8_t *glyph = font_get_glyph(current_text[char_index]);
        if (!glyph) continue;

        uint8_t column_bits = glyph[col_in_char];
        for (int row = 0; row < FONT_HEIGHT; row++) {
            if (column_bits & (1 << row)) {
                led_panel_set_pixel(row + 1, col, color_r, color_g, color_b);
            }
        }
    }

    led_panel_refresh();
}

void scroller_init(void)
{
    scroller_mutex = xSemaphoreCreateMutex();
}

int scroller_tick(bool *cycle_complete)
{
    xSemaphoreTake(scroller_mutex, portMAX_DELAY);
    render_frame();

    // Advance scroll position and detect cycle completion
    int text_len = strlen(current_text);
    bool done = false;
    if (text_len > 0) {
        int char_width = FONT_WIDTH + 1;
        int total_width = text_len * char_width + led_panel_get_cols();
        scroll_x = (scroll_x + 1) % total_width;
        // Cycle completes when scroll_x returns to initial position (blank gap)
        int initial_pos = text_len * char_width;
        done = (scroll_x == initial_pos);
    }

    uint8_t spd = scroll_speed;
    xSemaphoreGive(scroller_mutex);

    if (cycle_complete) *cycle_complete = done;

    int delay_ms = 98 - (spd * 8);
    if (delay_ms < 20) delay_ms = 20;
    return delay_ms;
}

void scroller_set_text(const char *text)
{
    xSemaphoreTake(scroller_mutex, portMAX_DELAY);
    strncpy(current_text, text, SCROLLER_MAX_TEXT_LEN);
    current_text[SCROLLER_MAX_TEXT_LEN] = '\0';
    scroll_x = strlen(current_text) * (FONT_WIDTH + 1);
    xSemaphoreGive(scroller_mutex);
    ESP_LOGI(TAG, "Text set to: %s", current_text);
}

void scroller_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    xSemaphoreTake(scroller_mutex, portMAX_DELAY);
    color_r = r;
    color_g = g;
    color_b = b;
    xSemaphoreGive(scroller_mutex);
}

void scroller_set_speed(uint8_t speed)
{
    if (speed < 1) speed = 1;
    if (speed > 10) speed = 10;
    xSemaphoreTake(scroller_mutex, portMAX_DELAY);
    scroll_speed = speed;
    xSemaphoreGive(scroller_mutex);
}

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
static uint16_t scroll_phase_q8 = 0;
static SemaphoreHandle_t scroller_mutex = NULL;

// Fixed frame timing improves visual smoothness.
#define SCROLLER_FRAME_MS 16
#define SCROLLER_Q8_ONE   256

// Pixels-per-frame in Q8 fixed-point (index 0 => speed 1).
// This gives finer speed granularity with a faster top end than delay-based stepping.
static const uint16_t speed_px_per_frame_q8[10] = {
    56,   // 0.22 px/frame
    72,   // 0.28 px/frame
    92,   // 0.36 px/frame
    116,  // 0.45 px/frame
    144,  // 0.56 px/frame
    176,  // 0.69 px/frame
    212,  // 0.83 px/frame
    252,  // 0.98 px/frame
    296,  // 1.16 px/frame
    344,  // 1.34 px/frame
};

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

    // Advance by fractional pixels (Q8), then commit whole-pixel steps.
    // This preserves smooth pacing while still rendering on pixel boundaries.
    int text_len = strlen(current_text);
    bool done = false;
    if (text_len > 0) {
        int char_width = FONT_WIDTH + 1;
        int total_width = text_len * char_width + led_panel_get_cols();
        int initial_pos = text_len * char_width;
        uint16_t step_q8 = speed_px_per_frame_q8[scroll_speed - 1];

        scroll_phase_q8 += step_q8;
        while (scroll_phase_q8 >= SCROLLER_Q8_ONE) {
            scroll_phase_q8 -= SCROLLER_Q8_ONE;
            scroll_x = (scroll_x + 1) % total_width;
            // Cycle completes when scroll_x returns to initial blank-gap position.
            if (scroll_x == initial_pos) {
                done = true;
            }
        }
    }

    xSemaphoreGive(scroller_mutex);

    if (cycle_complete) *cycle_complete = done;

    return SCROLLER_FRAME_MS;
}

void scroller_set_text(const char *text)
{
    xSemaphoreTake(scroller_mutex, portMAX_DELAY);
    strncpy(current_text, text, SCROLLER_MAX_TEXT_LEN);
    current_text[SCROLLER_MAX_TEXT_LEN] = '\0';
    scroll_x = strlen(current_text) * (FONT_WIDTH + 1);
    scroll_phase_q8 = 0;
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

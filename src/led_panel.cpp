#define FASTLED_LEAN_AND_MEAN 1
#include <FastLED.h>
#include <cstring>

#include "esp_log.h"

extern "C" {
#include "led_panel.h"
}

static const char *TAG = "led_panel";

static pixel_rgb_t framebuffer[PANEL_ROWS][PANEL_MAX_COLS];
static CRGB leds[PANEL_MAX_LEDS];
static uint8_t global_brightness = 32;
static uint8_t panel_cols = 32;
static bool initialized = false;

// Convert (row, col) to linear LED index for column-major serpentine layout.
// Data enters top-left, snakes down col 0, up col 1, down col 2, etc.
static inline int pixel_index(int row, int col) {
    if ((col & 1) == 0) {
        return col * PANEL_ROWS + row;
    }
    return col * PANEL_ROWS + (PANEL_ROWS - 1 - row);
}

static inline int active_led_count(void) {
    return panel_cols * PANEL_ROWS;
}

static void configure_fastled_controller(void) {
    FastLED.addLeds<WS2812B, LED_STRIP_GPIO, GRB>(leds, active_led_count());
    FastLED.setBrightness(global_brightness);
}

extern "C" esp_err_t led_panel_init(void) {
    configure_fastled_controller();

    std::memset(framebuffer, 0, sizeof(framebuffer));
    FastLED.clear(true);
    initialized = true;

    ESP_LOGI(TAG, "LED panel initialized (FastLED): %ux%u (%u LEDs) on GPIO %u",
             static_cast<unsigned>(panel_cols),
             static_cast<unsigned>(PANEL_ROWS),
             static_cast<unsigned>(active_led_count()),
             static_cast<unsigned>(LED_STRIP_GPIO));
    return ESP_OK;
}

extern "C" void led_panel_clear(void) {
    std::memset(framebuffer, 0, sizeof(framebuffer));
}

extern "C" void led_panel_set_pixel(int row, int col, uint8_t r, uint8_t g, uint8_t b) {
    if (row < 0 || row >= PANEL_ROWS || col < 0 || col >= panel_cols) {
        return;
    }
    framebuffer[row][col].r = r;
    framebuffer[row][col].g = g;
    framebuffer[row][col].b = b;
}

extern "C" pixel_rgb_t led_panel_get_pixel(int row, int col) {
    pixel_rgb_t black = {0, 0, 0};
    if (row < 0 || row >= PANEL_ROWS || col < 0 || col >= panel_cols) {
        return black;
    }
    return framebuffer[row][col];
}

extern "C" esp_err_t led_panel_refresh(void) {
    for (int row = 0; row < PANEL_ROWS; row++) {
        for (int col = 0; col < panel_cols; col++) {
            const pixel_rgb_t *px = &framebuffer[row][col];
            leds[pixel_index(row, col)] = CRGB(px->r, px->g, px->b);
        }
    }

    FastLED.show();
    return ESP_OK;
}

extern "C" void led_panel_set_brightness(uint8_t brightness) {
    global_brightness = brightness;
    FastLED.setBrightness(global_brightness);
}

extern "C" void led_panel_set_cols(uint8_t cols) {
    if (cols < 32) cols = 32;
    if (cols > PANEL_MAX_COLS) cols = PANEL_MAX_COLS;

    if (panel_cols == cols) {
        return;
    }

    panel_cols = cols;
    if (initialized) {
        configure_fastled_controller();
    }
}

extern "C" uint8_t led_panel_get_cols(void) {
    return panel_cols;
}

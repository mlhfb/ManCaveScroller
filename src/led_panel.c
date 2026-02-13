#include "led_panel.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"

static const char *TAG = "led_panel";

static pixel_rgb_t framebuffer[PANEL_ROWS][PANEL_MAX_COLS];
static uint8_t led_buffer[PANEL_MAX_LEDS * 3]; // GRB byte order
static uint8_t global_brightness = 32;
static uint8_t panel_cols = 32; // runtime panel width

static rmt_channel_handle_t rmt_channel = NULL;
static rmt_encoder_handle_t rmt_encoder = NULL;

// Convert (row, col) to linear LED index for column-major serpentine layout.
// Data enters top-left, snakes down col 0, up col 1, down col 2, etc.
// Even columns (0,2,4...): top to bottom (row 0 to 7)
// Odd columns (1,3,5...):  bottom to top (row 7 to 0)
static inline int pixel_index(int row, int col)
{
    if (col % 2 == 0) {
        return col * PANEL_ROWS + row;
    } else {
        return col * PANEL_ROWS + (PANEL_ROWS - 1 - row);
    }
}

esp_err_t led_panel_init(void)
{
    // Configure RMT TX channel
    rmt_tx_channel_config_t tx_config = {
        .gpio_num = LED_STRIP_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000, // 10 MHz = 100ns per tick
        .mem_block_symbols = 256,
        .trans_queue_depth = 4,
    };
    esp_err_t err = rmt_new_tx_channel(&tx_config, &rmt_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT TX channel: %s", esp_err_to_name(err));
        return err;
    }

    // Configure bytes encoder with WS2812B timing
    // At 10 MHz: 1 tick = 100ns
    // Bit 0: 3 ticks (300ns) high, 9 ticks (900ns) low
    // Bit 1: 9 ticks (900ns) high, 3 ticks (300ns) low
    rmt_bytes_encoder_config_t encoder_config = {
        .bit0 = {
            .duration0 = 3,
            .level0 = 1,
            .duration1 = 9,
            .level1 = 0,
        },
        .bit1 = {
            .duration0 = 9,
            .level0 = 1,
            .duration1 = 3,
            .level1 = 0,
        },
        .flags.msb_first = true,
    };
    err = rmt_new_bytes_encoder(&encoder_config, &rmt_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create RMT encoder: %s", esp_err_to_name(err));
        return err;
    }

    // Enable channel
    err = rmt_enable(rmt_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable RMT channel: %s", esp_err_to_name(err));
        return err;
    }

    // Suppress internal RMT driver error logs — we handle stalls ourselves
    esp_log_level_set("rmt", ESP_LOG_NONE);

    led_panel_clear();
    ESP_LOGI(TAG, "LED panel initialized: %dx%d (%d LEDs) on GPIO %d",
             panel_cols, PANEL_ROWS, panel_cols * PANEL_ROWS, LED_STRIP_GPIO);
    return ESP_OK;
}

void led_panel_clear(void)
{
    memset(framebuffer, 0, sizeof(framebuffer));
}

void led_panel_set_pixel(int row, int col, uint8_t r, uint8_t g, uint8_t b)
{
    if (row < 0 || row >= PANEL_ROWS || col < 0 || col >= panel_cols) {
        return;
    }
    framebuffer[row][col].r = r;
    framebuffer[row][col].g = g;
    framebuffer[row][col].b = b;
}

pixel_rgb_t led_panel_get_pixel(int row, int col)
{
    pixel_rgb_t black = {0, 0, 0};
    if (row < 0 || row >= PANEL_ROWS || col < 0 || col >= panel_cols) {
        return black;
    }
    return framebuffer[row][col];
}

esp_err_t led_panel_refresh(void)
{
    // Convert framebuffer to GRB byte buffer with brightness scaling and serpentine mapping
    for (int row = 0; row < PANEL_ROWS; row++) {
        for (int col = 0; col < panel_cols; col++) {
            int idx = pixel_index(row, col) * 3;
            pixel_rgb_t *px = &framebuffer[row][col];
            // WS2812B expects GRB order
            led_buffer[idx + 0] = (px->g * global_brightness) / 255;
            led_buffer[idx + 1] = (px->r * global_brightness) / 255;
            led_buffer[idx + 2] = (px->b * global_brightness) / 255;
        }
    }

    // Transmit via RMT
    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    size_t active_size = panel_cols * PANEL_ROWS * 3;
    esp_err_t err = rmt_transmit(rmt_channel, rmt_encoder, led_buffer, active_size, &tx_config);
    if (err != ESP_OK) return err;

    rmt_tx_wait_all_done(rmt_channel, pdMS_TO_TICKS(100));
    return ESP_OK;
}

void led_panel_set_brightness(uint8_t brightness)
{
    global_brightness = brightness;
}

void led_panel_set_cols(uint8_t cols)
{
    if (cols < 32) cols = 32;
    if (cols > PANEL_MAX_COLS) cols = PANEL_MAX_COLS;
    panel_cols = cols;
}

uint8_t led_panel_get_cols(void)
{
    return panel_cols;
}

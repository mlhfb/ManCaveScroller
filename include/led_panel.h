#ifndef LED_PANEL_H
#define LED_PANEL_H

#include <stdint.h>
#include "esp_err.h"

#define PANEL_ROWS     8
#define PANEL_MAX_COLS 128  // max 4 panels of 32
#define PANEL_MAX_LEDS (PANEL_ROWS * PANEL_MAX_COLS)

#ifndef LED_STRIP_GPIO
#define LED_STRIP_GPIO 13
#endif

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pixel_rgb_t;

esp_err_t led_panel_init(void);
void led_panel_clear(void);
void led_panel_set_pixel(int row, int col, uint8_t r, uint8_t g, uint8_t b);
pixel_rgb_t led_panel_get_pixel(int row, int col);
esp_err_t led_panel_refresh(void);
void led_panel_set_brightness(uint8_t brightness);
void led_panel_set_cols(uint8_t cols);
uint8_t led_panel_get_cols(void);

#endif

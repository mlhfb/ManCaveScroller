#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define SETTINGS_MAX_TEXT_LEN     200
#define SETTINGS_MAX_SSID_LEN    32
#define SETTINGS_MAX_PASS_LEN    64
#define MAX_MESSAGES              5

typedef struct {
    char text[SETTINGS_MAX_TEXT_LEN + 1];
    uint8_t color_r;
    uint8_t color_g;
    uint8_t color_b;
    bool enabled;
} message_t;

typedef struct {
    message_t messages[MAX_MESSAGES];
    uint8_t speed;       // 1-10
    uint8_t brightness;  // 0-255
    char wifi_ssid[SETTINGS_MAX_SSID_LEN + 1];
    char wifi_password[SETTINGS_MAX_PASS_LEN + 1];
} app_settings_t;

esp_err_t settings_init(void);
esp_err_t settings_save(const app_settings_t *settings);
app_settings_t *settings_get(void);

#endif

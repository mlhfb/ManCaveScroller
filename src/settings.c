#include "settings.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "mancave";

static app_settings_t current_settings;

static const app_settings_t default_settings = {
    .text = "Hello Man Cave!",
    .color_r = 255,
    .color_g = 0,
    .color_b = 0,
    .speed = 5,
    .brightness = 32,
    .wifi_ssid = "",
    .wifi_password = "",
};

static void load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved settings, using defaults");
        memcpy(&current_settings, &default_settings, sizeof(app_settings_t));
        return;
    }

    // Start with defaults, then override with stored values
    memcpy(&current_settings, &default_settings, sizeof(app_settings_t));

    size_t len = sizeof(current_settings.text);
    nvs_get_str(handle, "text", current_settings.text, &len);

    nvs_get_u8(handle, "color_r", &current_settings.color_r);
    nvs_get_u8(handle, "color_g", &current_settings.color_g);
    nvs_get_u8(handle, "color_b", &current_settings.color_b);
    nvs_get_u8(handle, "speed", &current_settings.speed);
    nvs_get_u8(handle, "bright", &current_settings.brightness);

    len = sizeof(current_settings.wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", current_settings.wifi_ssid, &len);

    len = sizeof(current_settings.wifi_password);
    nvs_get_str(handle, "wifi_pass", current_settings.wifi_password, &len);

    nvs_close(handle);
    ESP_LOGI(TAG, "Settings loaded from NVS");
}

esp_err_t settings_init(void)
{
    load_from_nvs();
    return ESP_OK;
}

esp_err_t settings_save(const app_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return err;
    }

    memcpy(&current_settings, settings, sizeof(app_settings_t));

    nvs_set_str(handle, "text", current_settings.text);
    nvs_set_u8(handle, "color_r", current_settings.color_r);
    nvs_set_u8(handle, "color_g", current_settings.color_g);
    nvs_set_u8(handle, "color_b", current_settings.color_b);
    nvs_set_u8(handle, "speed", current_settings.speed);
    nvs_set_u8(handle, "bright", current_settings.brightness);
    nvs_set_str(handle, "wifi_ssid", current_settings.wifi_ssid);
    nvs_set_str(handle, "wifi_pass", current_settings.wifi_password);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Settings saved to NVS");
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
    }
    return err;
}

app_settings_t *settings_get(void)
{
    return &current_settings;
}

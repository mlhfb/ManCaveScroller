#include "settings.h"
#include <string.h>
#include <stdio.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "mancave";

static app_settings_t current_settings;

static const app_settings_t default_settings = {
    .messages = {
        { .text = "Hello Man Cave!", .color_r = 255, .color_g = 0, .color_b = 0, .enabled = true },
        { .text = "", .color_r = 0, .color_g = 255, .color_b = 0, .enabled = false },
        { .text = "", .color_r = 0, .color_g = 0, .color_b = 255, .enabled = false },
        { .text = "", .color_r = 255, .color_g = 255, .color_b = 0, .enabled = false },
        { .text = "", .color_r = 255, .color_g = 0, .color_b = 255, .enabled = false },
    },
    .speed = 5,
    .brightness = 32,
    .panel_cols = 32,
    .wifi_ssid = "",
    .wifi_password = "",
    .rss_enabled = true,
    .rss_url = "https://feeds.npr.org/1001/rss.xml",
    .rss_source_count = 0,  // normalized from legacy rss_enabled/rss_url
};

static void normalize_rss_sources(app_settings_t *s)
{
    if (s->rss_source_count > MAX_RSS_SOURCES) {
        s->rss_source_count = MAX_RSS_SOURCES;
    }

    bool has_configured_source = false;
    for (int i = 0; i < s->rss_source_count; i++) {
        if (s->rss_sources[i].enabled && strlen(s->rss_sources[i].url) > 0) {
            has_configured_source = true;
            break;
        }
    }

    // Backward-compatible path: if no source array configured, map legacy rss settings to slot 0.
    if (!has_configured_source) {
        s->rss_source_count = 1;
        memset(&s->rss_sources[0], 0, sizeof(s->rss_sources[0]));
        s->rss_sources[0].enabled = s->rss_enabled && strlen(s->rss_url) > 0;
        strncpy(s->rss_sources[0].name, "Primary RSS", SETTINGS_MAX_RSS_NAME_LEN);
        s->rss_sources[0].name[SETTINGS_MAX_RSS_NAME_LEN] = '\0';
        strncpy(s->rss_sources[0].url, s->rss_url, SETTINGS_MAX_URL_LEN);
        s->rss_sources[0].url[SETTINGS_MAX_URL_LEN] = '\0';
    }

    // Keep legacy fields mirrored to source slot 0 for current API compatibility.
    s->rss_enabled = s->rss_sources[0].enabled;
    strncpy(s->rss_url, s->rss_sources[0].url, SETTINGS_MAX_URL_LEN);
    s->rss_url[SETTINGS_MAX_URL_LEN] = '\0';
}

static void load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved settings, using defaults");
        memcpy(&current_settings, &default_settings, sizeof(app_settings_t));
        return;
    }

    // Start with defaults, then override with stored values
    memcpy(&current_settings, &default_settings, sizeof(app_settings_t));

    // Migrate old single-message format if present
    char old_text[SETTINGS_MAX_TEXT_LEN + 1] = "";
    size_t len = sizeof(old_text);
    if (nvs_get_str(handle, "text", old_text, &len) == ESP_OK && strlen(old_text) > 0) {
        ESP_LOGI(TAG, "Migrating old single-message to messages[0]");
        strncpy(current_settings.messages[0].text, old_text, SETTINGS_MAX_TEXT_LEN);
        current_settings.messages[0].enabled = true;
        nvs_get_u8(handle, "color_r", &current_settings.messages[0].color_r);
        nvs_get_u8(handle, "color_g", &current_settings.messages[0].color_g);
        nvs_get_u8(handle, "color_b", &current_settings.messages[0].color_b);
        // Remove old keys after migration
        nvs_erase_key(handle, "text");
        nvs_erase_key(handle, "color_r");
        nvs_erase_key(handle, "color_g");
        nvs_erase_key(handle, "color_b");
        nvs_commit(handle);
    }

    // Load messages array
    for (int i = 0; i < MAX_MESSAGES; i++) {
        char key[16];

        snprintf(key, sizeof(key), "msg%d_text", i);
        len = sizeof(current_settings.messages[i].text);
        nvs_get_str(handle, key, current_settings.messages[i].text, &len);

        snprintf(key, sizeof(key), "msg%d_r", i);
        nvs_get_u8(handle, key, &current_settings.messages[i].color_r);

        snprintf(key, sizeof(key), "msg%d_g", i);
        nvs_get_u8(handle, key, &current_settings.messages[i].color_g);

        snprintf(key, sizeof(key), "msg%d_b", i);
        nvs_get_u8(handle, key, &current_settings.messages[i].color_b);

        snprintf(key, sizeof(key), "msg%d_en", i);
        uint8_t en = current_settings.messages[i].enabled ? 1 : 0;
        nvs_get_u8(handle, key, &en);
        current_settings.messages[i].enabled = (en != 0);
    }

    nvs_get_u8(handle, "speed", &current_settings.speed);
    nvs_get_u8(handle, "bright", &current_settings.brightness);
    nvs_get_u8(handle, "panel_cols", &current_settings.panel_cols);

    len = sizeof(current_settings.wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", current_settings.wifi_ssid, &len);

    len = sizeof(current_settings.wifi_password);
    nvs_get_str(handle, "wifi_pass", current_settings.wifi_password, &len);

    uint8_t rss_en = current_settings.rss_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_en", &rss_en);
    current_settings.rss_enabled = (rss_en != 0);

    len = sizeof(current_settings.rss_url);
    nvs_get_str(handle, "rss_url", current_settings.rss_url, &len);

    // Load source list (future multi-source support).
    nvs_get_u8(handle, "rss_count", &current_settings.rss_source_count);
    if (current_settings.rss_source_count > MAX_RSS_SOURCES) {
        current_settings.rss_source_count = MAX_RSS_SOURCES;
    }
    for (int i = 0; i < current_settings.rss_source_count; i++) {
        char key[20];

        snprintf(key, sizeof(key), "rs%d_en", i);
        uint8_t en = current_settings.rss_sources[i].enabled ? 1 : 0;
        nvs_get_u8(handle, key, &en);
        current_settings.rss_sources[i].enabled = (en != 0);

        snprintf(key, sizeof(key), "rs%d_name", i);
        len = sizeof(current_settings.rss_sources[i].name);
        nvs_get_str(handle, key, current_settings.rss_sources[i].name, &len);

        snprintf(key, sizeof(key), "rs%d_url", i);
        len = sizeof(current_settings.rss_sources[i].url);
        nvs_get_str(handle, key, current_settings.rss_sources[i].url, &len);
    }

    normalize_rss_sources(&current_settings);

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

    // Current API still writes legacy rss_enabled/rss_url fields. Mirror those into slot 0.
    if (current_settings.rss_source_count == 0 || current_settings.rss_source_count > MAX_RSS_SOURCES) {
        current_settings.rss_source_count = 1;
    }
    current_settings.rss_sources[0].enabled = current_settings.rss_enabled;
    if (current_settings.rss_sources[0].name[0] == '\0') {
        strncpy(current_settings.rss_sources[0].name, "Primary RSS", SETTINGS_MAX_RSS_NAME_LEN);
        current_settings.rss_sources[0].name[SETTINGS_MAX_RSS_NAME_LEN] = '\0';
    }
    strncpy(current_settings.rss_sources[0].url, current_settings.rss_url, SETTINGS_MAX_URL_LEN);
    current_settings.rss_sources[0].url[SETTINGS_MAX_URL_LEN] = '\0';
    normalize_rss_sources(&current_settings);

    for (int i = 0; i < MAX_MESSAGES; i++) {
        char key[16];

        snprintf(key, sizeof(key), "msg%d_text", i);
        nvs_set_str(handle, key, current_settings.messages[i].text);

        snprintf(key, sizeof(key), "msg%d_r", i);
        nvs_set_u8(handle, key, current_settings.messages[i].color_r);

        snprintf(key, sizeof(key), "msg%d_g", i);
        nvs_set_u8(handle, key, current_settings.messages[i].color_g);

        snprintf(key, sizeof(key), "msg%d_b", i);
        nvs_set_u8(handle, key, current_settings.messages[i].color_b);

        snprintf(key, sizeof(key), "msg%d_en", i);
        nvs_set_u8(handle, key, current_settings.messages[i].enabled ? 1 : 0);
    }

    nvs_set_u8(handle, "speed", current_settings.speed);
    nvs_set_u8(handle, "bright", current_settings.brightness);
    nvs_set_u8(handle, "panel_cols", current_settings.panel_cols);
    nvs_set_str(handle, "wifi_ssid", current_settings.wifi_ssid);
    nvs_set_str(handle, "wifi_pass", current_settings.wifi_password);
    nvs_set_u8(handle, "rss_en", current_settings.rss_enabled ? 1 : 0);
    esp_err_t url_err = nvs_set_str(handle, "rss_url", current_settings.rss_url);
    if (url_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save rss_url: %s", esp_err_to_name(url_err));
    }
    nvs_set_u8(handle, "rss_count", current_settings.rss_source_count);
    for (int i = 0; i < current_settings.rss_source_count; i++) {
        char key[20];

        snprintf(key, sizeof(key), "rs%d_en", i);
        nvs_set_u8(handle, key, current_settings.rss_sources[i].enabled ? 1 : 0);

        snprintf(key, sizeof(key), "rs%d_name", i);
        nvs_set_str(handle, key, current_settings.rss_sources[i].name);

        snprintf(key, sizeof(key), "rs%d_url", i);
        nvs_set_str(handle, key, current_settings.rss_sources[i].url);
    }

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

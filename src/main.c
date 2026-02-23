#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_littlefs.h"
#include "driver/gpio.h"
#include "led_panel.h"
#include "font.h"
#include "settings.h"
#include "storage_paths.h"
#include "text_scroller.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "rss_fetcher.h"
#include "rss_cache.h"

static const char *TAG = "main";

#define CONFIG_BUTTON_GPIO 0

#define RSS_REFRESH_INTERVAL_MS (15 * 60 * 1000)
#define RSS_REFRESH_RETRY_MS    (60 * 1000)

static volatile bool config_button_pressed = false;
static volatile uint32_t last_button_tick = 0;

static bool rss_have_item = false;
static rss_item_t rss_item;
static int rss_item_source_idx = -1;
static bool rss_item_live = false;
static bool rss_showing_title = true;
static TickType_t rss_next_refresh_tick = 0;

static const uint8_t rss_colors[][3] = {
    {255, 255, 255},
    {255, 255, 0},
    {0,   255, 0},
    {255, 0,   0},
    {0,   0,   255},
    {0,   255, 255},
    {148, 0,   211},
};
#define RSS_NUM_COLORS 7

static void IRAM_ATTR button_isr_handler(void *arg)
{
    (void)arg;
    uint32_t now = xTaskGetTickCountFromISR();
    if ((now - last_button_tick) > pdMS_TO_TICKS(300)) {
        config_button_pressed = true;
        last_button_tick = now;
    }
}

static void config_button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONFIG_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CONFIG_BUTTON_GPIO, button_isr_handler, NULL);
}

static esp_err_t littlefs_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = LITTLEFS_BASE_PATH,
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t total = 0;
    size_t used = 0;
    err = esp_littlefs_info(conf.partition_label, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "LittleFS mounted: %u KB total, %u KB used",
                 (unsigned)(total / 1024), (unsigned)(used / 1024));
    }
    return ESP_OK;
}

static int next_enabled_message(const app_settings_t *s, int current)
{
    for (int i = 1; i <= MAX_MESSAGES; i++) {
        int idx = (current + i) % MAX_MESSAGES;
        if (s->messages[idx].enabled && strlen(s->messages[idx].text) > 0) {
            return idx;
        }
    }
    return -1;
}

static void load_message(const app_settings_t *s, int idx)
{
    scroller_set_text(s->messages[idx].text);
    scroller_set_color(s->messages[idx].color_r,
                       s->messages[idx].color_g,
                       s->messages[idx].color_b);
}

static void load_custom_or_prompt(const app_settings_t *s, int *current_msg, const char *fallback)
{
    *current_msg = next_enabled_message(s, MAX_MESSAGES - 1);
    if (*current_msg >= 0) {
        load_message(s, *current_msg);
    } else {
        scroller_set_text(fallback);
    }
}

static int rss_source_count(const app_settings_t *s)
{
    int count = s->rss_source_count;
    if (count > MAX_RSS_SOURCES) count = MAX_RSS_SOURCES;
    return count;
}

static bool rss_source_enabled(const app_settings_t *s, int idx)
{
    int count = rss_source_count(s);
    if (idx < 0 || idx >= count) return false;
    return s->rss_sources[idx].enabled && strlen(s->rss_sources[idx].url) > 0;
}

static bool rss_sources_available(const app_settings_t *s)
{
    if (!s->rss_enabled) return false;
    int count = rss_source_count(s);
    for (int i = 0; i < count; i++) {
        if (rss_source_enabled(s, i)) return true;
    }
    return false;
}

static void rss_playback_reset(void)
{
    rss_have_item = false;
    memset(&rss_item, 0, sizeof(rss_item));
    rss_item_source_idx = -1;
    rss_item_live = false;
    rss_showing_title = true;
}

static int collect_enabled_source_urls(const app_settings_t *s,
                                       const char **urls,
                                       int *source_indices,
                                       int max_items)
{
    if (!s || !urls || !source_indices || max_items <= 0) return 0;

    int count = rss_source_count(s);
    int out_count = 0;
    for (int i = 0; i < count && out_count < max_items; i++) {
        if (!rss_source_enabled(s, i)) continue;
        urls[out_count] = s->rss_sources[i].url;
        source_indices[out_count] = i;
        out_count++;
    }
    return out_count;
}

static bool rss_cache_available_for_enabled_sources(const app_settings_t *s)
{
    int count = rss_source_count(s);
    for (int i = 0; i < count; i++) {
        if (!rss_source_enabled(s, i)) continue;
        if (rss_cache_has_items_for_url(s->rss_sources[i].url)) {
            return true;
        }
    }
    return false;
}

static bool rss_pick_random_cached_item(const app_settings_t *s)
{
    const char *urls[MAX_RSS_SOURCES] = {0};
    int source_indices[MAX_RSS_SOURCES] = {0};
    int enabled_count = collect_enabled_source_urls(s, urls, source_indices, MAX_RSS_SOURCES);
    if (enabled_count <= 0) return false;

    rss_item_t selected = {0};
    int selected_enabled_idx = -1;
    uint8_t selected_flags = 0;
    bool cycle_reset = false;
    esp_err_t err = rss_cache_pick_random_item_ex(urls,
                                                  enabled_count,
                                                  &selected,
                                                  &selected_enabled_idx,
                                                  &selected_flags,
                                                  &cycle_reset);
    if (err != ESP_OK) {
        return false;
    }

    if (cycle_reset) {
        ESP_LOGI(TAG, "RSS random cycle exhausted; restarting pool");
    }

    rss_item = selected;
    rss_have_item = true;
    rss_item_live = (selected_flags & RSS_CACHE_ITEM_FLAG_LIVE) != 0;
    rss_showing_title = true;
    if (selected_enabled_idx >= 0 && selected_enabled_idx < enabled_count) {
        rss_item_source_idx = source_indices[selected_enabled_idx];
    } else {
        rss_item_source_idx = 0;
    }
    return true;
}

static bool rss_show_current_item_segment(void)
{
    if (!rss_have_item) return false;

    int color_idx = (rss_item_source_idx >= 0) ? rss_item_source_idx : 0;
    const uint8_t *color = rss_colors[color_idx % RSS_NUM_COLORS];
    scroller_set_color(color[0], color[1], color[2]);

    if (rss_showing_title) {
        // Future hot-list scheduling can use rss_item_live to prioritize in-progress games.
        if (rss_item_live) {
            ESP_LOGD(TAG, "Showing LIVE feed item from source index %d", rss_item_source_idx);
        }
        scroller_set_text(rss_item.title[0] ? rss_item.title : "(no title)");
        rss_showing_title = false;
    } else {
        scroller_set_text(rss_item.description[0] ? rss_item.description : "(no description)");
        rss_showing_title = true;
        rss_have_item = false;
    }

    return true;
}

static bool rss_prepare_next_display_item(const app_settings_t *s)
{
    if (!rss_have_item && !rss_pick_random_cached_item(s)) {
        return false;
    }
    return rss_show_current_item_segment();
}

static bool rss_refresh_cache(const app_settings_t *s)
{
    if (!rss_sources_available(s)) {
        return false;
    }
    if (wifi_manager_get_mode() != WIFI_MGR_MODE_STA) {
        return rss_cache_available_for_enabled_sources(s);
    }

    scroller_set_text("Updating feeds...");
    scroller_set_color(255, 255, 255);
    scroller_tick(NULL);

    if (!wifi_manager_radio_on()) {
        ESP_LOGW(TAG, "WiFi connect failed for RSS refresh");
        wifi_manager_radio_off();
        return rss_cache_available_for_enabled_sources(s);
    }

    int count = rss_source_count(s);
    int fetched_sources = 0;
    int cached_sources = 0;

    for (int i = 0; i < count; i++) {
        if (!rss_source_enabled(s, i)) continue;

        ESP_LOGI(TAG, "Refreshing source %d/%d: %s", i + 1, count, s->rss_sources[i].name);
        esp_err_t fetch_err = rss_fetch(s->rss_sources[i].url);
        if (fetch_err == ESP_OK && rss_get_count() > 0) {
            fetched_sources++;
            esp_err_t cache_err = rss_cache_store_from_fetcher(
                s->rss_sources[i].url, s->rss_sources[i].name);
            if (cache_err == ESP_OK) {
                cached_sources++;
            } else {
                ESP_LOGW(TAG, "Cache write failed for '%s': %s",
                         s->rss_sources[i].name, esp_err_to_name(cache_err));
            }
        } else {
            ESP_LOGW(TAG, "Feed refresh failed for '%s': %s",
                     s->rss_sources[i].name, esp_err_to_name(fetch_err));
        }
    }

    wifi_manager_radio_off();

    bool cache_ready = rss_cache_available_for_enabled_sources(s);
    ESP_LOGI(TAG, "RSS refresh complete: fetched=%d cached=%d cache_ready=%d",
             fetched_sources, cached_sources, cache_ready);
    return cache_ready;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ManCaveScroller starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    littlefs_init();
    if (rss_cache_init() != ESP_OK) {
        ESP_LOGW(TAG, "RSS cache init failed");
    }

    settings_init();
    app_settings_t *settings = settings_get();

    ret = led_panel_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED panel init failed!");
        return;
    }
    led_panel_set_brightness(settings->brightness);
    led_panel_set_cols(settings->panel_cols);

    font_init();
    scroller_init();
    scroller_set_speed(settings->speed);

    wifi_manager_init();
    wifi_manager_start();
    web_server_start();

    rss_playback_reset();
    bool rss_active = false;

    int current_msg = -1;
    wifi_mgr_mode_t mode = wifi_manager_get_mode();
    ESP_LOGI(TAG, "WiFi mode=%d, rss_enabled=%d, rss_sources=%d",
             mode, settings->rss_enabled, settings->rss_source_count);

    if (mode == WIFI_MGR_MODE_STA && rss_sources_available(settings)) {
        rss_active = rss_refresh_cache(settings);
        if (rss_active) {
            rss_active = rss_prepare_next_display_item(settings);
        }
        TickType_t now = xTaskGetTickCount();
        rss_next_refresh_tick = now + pdMS_TO_TICKS(
            rss_active ? RSS_REFRESH_INTERVAL_MS : RSS_REFRESH_RETRY_MS);
    }

    if (!rss_active) {
        load_custom_or_prompt(settings, &current_msg,
                              "No messages     Press button to configure");
    }

    config_button_init();
    bool config_mode = false;

    ESP_LOGI(TAG, "ManCaveScroller ready - press BOOT for config mode");

    while (1) {
        if (config_button_pressed) {
            config_button_pressed = false;

            if (!config_mode && wifi_manager_get_mode() == WIFI_MGR_MODE_STA) {
                ESP_LOGI(TAG, "BOOT: entering config mode");
                config_mode = true;
                if (wifi_manager_radio_on()) {
                    web_server_start();
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Config Mode     %s", wifi_manager_get_ip());
                    scroller_set_text(msg);
                } else {
                    scroller_set_text("Config Mode     WiFi failed");
                }
            } else if (config_mode) {
                ESP_LOGI(TAG, "BOOT: exiting config mode");
                config_mode = false;
                web_server_stop();
                wifi_manager_radio_off();

                settings = settings_get();
                scroller_set_speed(settings->speed);
                led_panel_set_brightness(settings->brightness);
                led_panel_set_cols(settings->panel_cols);

                rss_active = false;
                rss_playback_reset();
                if (wifi_manager_get_mode() == WIFI_MGR_MODE_STA && rss_sources_available(settings)) {
                    rss_active = rss_refresh_cache(settings);
                    if (rss_active) {
                        rss_active = rss_prepare_next_display_item(settings);
                    }
                    TickType_t now = xTaskGetTickCount();
                    rss_next_refresh_tick = now + pdMS_TO_TICKS(
                        rss_active ? RSS_REFRESH_INTERVAL_MS : RSS_REFRESH_RETRY_MS);
                }

                if (!rss_active) {
                    load_custom_or_prompt(settings, &current_msg,
                                          "RSS cache unavailable     Press button to configure");
                }
            }
        }

        bool cycle_done = false;
        int delay_ms = scroller_tick(&cycle_done);

        if (cycle_done && !config_mode) {
            settings = settings_get();

            if (wifi_manager_get_mode() == WIFI_MGR_MODE_STA && rss_sources_available(settings)) {
                TickType_t now = xTaskGetTickCount();
                if ((int32_t)(now - rss_next_refresh_tick) >= 0) {
                    bool cache_ready = rss_refresh_cache(settings);
                    if (cache_ready && !rss_active) {
                        rss_playback_reset();
                        rss_active = rss_prepare_next_display_item(settings);
                    }
                    rss_next_refresh_tick = now + pdMS_TO_TICKS(
                        cache_ready ? RSS_REFRESH_INTERVAL_MS : RSS_REFRESH_RETRY_MS);
                }
            }

            if (rss_active) {
                if (!rss_prepare_next_display_item(settings)) {
                    rss_active = false;
                    load_custom_or_prompt(settings, &current_msg,
                                          "RSS cache unavailable     Press button to configure");
                }
            } else {
                int next = next_enabled_message(settings, current_msg);
                if (next >= 0) {
                    current_msg = next;
                    load_message(settings, current_msg);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

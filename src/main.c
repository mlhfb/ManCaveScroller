#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "led_panel.h"
#include "settings.h"
#include "text_scroller.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "rss_fetcher.h"

static const char *TAG = "main";

#define CONFIG_BUTTON_GPIO 0  // BOOT button on ESP32 DevKit

static volatile bool config_button_pressed = false;
static volatile uint32_t last_button_tick = 0;

// Debounced BOOT button ISR (falling edge).
static void IRAM_ATTR button_isr_handler(void *arg)
{
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

// Find next enabled message index, wrapping around. Returns -1 if none enabled.
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

// Load a custom message into the scroller by index.
static void load_message(const app_settings_t *s, int idx)
{
    scroller_set_text(s->messages[idx].text);
    scroller_set_color(s->messages[idx].color_r,
                       s->messages[idx].color_g,
                       s->messages[idx].color_b);
}

// Load first enabled custom message, or a fallback prompt if none exist.
static void load_custom_or_prompt(const app_settings_t *s, int *current_msg, const char *fallback)
{
    *current_msg = next_enabled_message(s, MAX_MESSAGES - 1);
    if (*current_msg >= 0) {
        load_message(s, *current_msg);
    } else {
        scroller_set_text(fallback);
    }
}

// ── RSS color rotation ──
// Applied per RSS item as sources cycle.
static const uint8_t rss_colors[][3] = {
    {255, 255, 255},  // white
    {255, 255, 0},    // yellow
    {0,   255, 0},    // green
    {255, 0,   0},    // red
    {0,   0,   255},  // blue
    {0,   255, 255},  // cyan
    {148, 0,   211},  // violet
};
#define RSS_NUM_COLORS 7

// ── RSS scheduler state ──
//
// RSS scheduler tuning:
// - one source is active in memory at a time
// - failed sources retry with exponential backoff
#define RSS_RETRY_BASE_MS 30000
#define RSS_RETRY_MAX_MS  (10 * 60 * 1000)
#define RSS_IDLE_RETRY_MS 10000

// RSS playback and scheduler runtime state.
static int rss_source_idx = -1;
static int rss_item_idx = 0;
static bool rss_showing_title = true;
static int rss_color_idx = 0;
static TickType_t rss_next_retry_tick[MAX_RSS_SOURCES];
static uint32_t rss_retry_backoff_ms[MAX_RSS_SOURCES];
static TickType_t rss_next_scheduler_tick = 0;

// Normalize configured source count to a usable range.
static int rss_source_count(const app_settings_t *s)
{
    int count = s->rss_source_count;
    if (count < 1 || count > MAX_RSS_SOURCES) count = 1;
    return count;
}

// Source must be enabled and have a non-empty URL.
static bool rss_source_enabled(const app_settings_t *s, int idx)
{
    int count = rss_source_count(s);
    if (idx < 0 || idx >= count) return false;
    return s->rss_sources[idx].enabled && strlen(s->rss_sources[idx].url) > 0;
}

// Global RSS switch + at least one enabled source.
static bool rss_sources_available(const app_settings_t *s)
{
    if (!s->rss_enabled) return false;
    int count = rss_source_count(s);
    for (int i = 0; i < count; i++) {
        if (rss_source_enabled(s, i)) return true;
    }
    return false;
}

// Reset scheduler runtime state (used at boot and when exiting config mode).
static void rss_scheduler_reset(void)
{
    rss_source_idx = -1;
    rss_item_idx = 0;
    rss_showing_title = true;
    rss_color_idx = 0;
    rss_next_scheduler_tick = 0;
    for (int i = 0; i < MAX_RSS_SOURCES; i++) {
        rss_next_retry_tick[i] = 0;
        rss_retry_backoff_ms[i] = RSS_RETRY_BASE_MS;
    }
}

// Exponential backoff for source fetch failures.
static void rss_mark_fetch_failure(int idx, TickType_t now)
{
    if (idx < 0 || idx >= MAX_RSS_SOURCES) return;

    uint32_t backoff = rss_retry_backoff_ms[idx];
    if (backoff < RSS_RETRY_BASE_MS) {
        backoff = RSS_RETRY_BASE_MS;
    } else {
        uint32_t doubled = backoff * 2;
        backoff = (doubled > RSS_RETRY_MAX_MS) ? RSS_RETRY_MAX_MS : doubled;
    }

    rss_retry_backoff_ms[idx] = backoff;
    rss_next_retry_tick[idx] = now + pdMS_TO_TICKS(backoff);
}

// Clear retry delay when a source fetch succeeds.
static void rss_mark_fetch_success(int idx)
{
    if (idx < 0 || idx >= MAX_RSS_SOURCES) return;
    rss_retry_backoff_ms[idx] = RSS_RETRY_BASE_MS;
    rss_next_retry_tick[idx] = 0;
}

// Compute next scheduler wake time based on per-source retry windows.
static TickType_t rss_earliest_ready_time(const app_settings_t *s, TickType_t fallback)
{
    int count = rss_source_count(s);
    TickType_t earliest = 0;
    bool found = false;

    for (int i = 0; i < count; i++) {
        if (!rss_source_enabled(s, i)) continue;
        TickType_t t = rss_next_retry_tick[i];
        if (!found || (int32_t)(t - earliest) < 0) {
            earliest = t;
            found = true;
        }
    }

    return found ? earliest : fallback;
}

// Deterministic round-robin source selection from start_idx, respecting retry timing.
static int rss_next_ready_source(const app_settings_t *s, int start_idx, TickType_t now)
{
    int count = rss_source_count(s);
    for (int i = 1; i <= count; i++) {
        int idx = (start_idx + i) % count;
        if (!rss_source_enabled(s, idx)) continue;
        if ((int32_t)(now - rss_next_retry_tick[idx]) >= 0) {
            return idx;
        }
    }
    return -1;
}

// Fetch one RSS source:
// 1) show status
// 2) temporarily enable WiFi radio
// 3) fetch into shared rss_fetcher buffer
// 4) disable WiFi radio for glitch-free scrolling
static bool fetch_rss_source(const char *url, int source_idx)
{
    ESP_LOGI(TAG, "Fetching RSS source %d: %.60s", source_idx + 1, url);
    scroller_set_text("Updating news...");
    scroller_set_color(255, 255, 255);
    scroller_tick(NULL);

    if (!wifi_manager_radio_on()) {
        ESP_LOGW(TAG, "WiFi connect failed for RSS source %d", source_idx + 1);
        wifi_manager_radio_off();
        return false;
    }

    esp_err_t err = rss_fetch(url);
    wifi_manager_radio_off();

    if (err != ESP_OK || rss_get_count() == 0) {
        ESP_LOGW(TAG, "RSS source %d fetch failed or empty", source_idx + 1);
        return false;
    }

    ESP_LOGI(TAG, "RSS source %d loaded: %d items", source_idx + 1, rss_get_count());
    return true;
}

// Load current title/description into scroller.
// Returns false when current source has no more items.
static bool rss_load_current_item(void)
{
    if (rss_item_idx >= rss_get_count()) return false;

    const rss_item_t *item = rss_get_item(rss_item_idx);
    if (!item) return false;

    const uint8_t *color = rss_colors[rss_color_idx % RSS_NUM_COLORS];
    scroller_set_color(color[0], color[1], color[2]);

    if (rss_showing_title) {
        scroller_set_text(item->title[0] ? item->title : "(no title)");
        rss_showing_title = false;
    } else {
        scroller_set_text(item->description[0] ? item->description : "(no description)");
        rss_showing_title = true;
        rss_item_idx++;
        rss_color_idx = (rss_color_idx + 1) % RSS_NUM_COLORS;
    }

    return true;
}

// Activate the next ready source in round-robin order.
// On success: primes first item for display.
// On failure: schedules next retry point and returns false.
static bool rss_activate_next_source(const app_settings_t *s)
{
    TickType_t now = xTaskGetTickCount();
    int count = rss_source_count(s);
    int cursor = rss_source_idx;

    for (int attempt = 0; attempt < count; attempt++) {
        int idx = rss_next_ready_source(s, cursor, now);
        if (idx < 0) break;
        cursor = idx;

        if (fetch_rss_source(s->rss_sources[idx].url, idx)) {
            rss_mark_fetch_success(idx);
            rss_source_idx = idx;
            rss_item_idx = 0;
            rss_showing_title = true;
            rss_color_idx = 0;
            rss_next_scheduler_tick = now;
            return rss_load_current_item();
        }

        rss_mark_fetch_failure(idx, now);
    }

    rss_next_scheduler_tick = rss_earliest_ready_time(
        s, now + pdMS_TO_TICKS(RSS_IDLE_RETRY_MS));
    return false;
}

void app_main(void)
{
    ESP_LOGI(TAG, "ManCaveScroller starting...");

    // Initialize NVS flash.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Load settings from NVS.
    settings_init();
    app_settings_t *settings = settings_get();

    // Initialize LED panel.
    ret = led_panel_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED panel init failed!");
        return;
    }
    led_panel_set_brightness(settings->brightness);
    led_panel_set_cols(settings->panel_cols);

    // Initialize scroller.
    scroller_init();
    scroller_set_speed(settings->speed);

    // Start WiFi/web stack (WiFi manager decides AP/STA).
    wifi_manager_init();
    wifi_manager_start();
    web_server_start();

    // RSS scheduler state.
    rss_scheduler_reset();
    bool rss_active = false;

    // Initial content setup.
    int current_msg = -1;
    wifi_mgr_mode_t mode = wifi_manager_get_mode();
    ESP_LOGI(TAG, "WiFi mode=%d, rss_enabled=%d, rss_sources=%d",
             mode, settings->rss_enabled, settings->rss_source_count);

    // In STA mode, try to activate first available RSS source.
    if (mode == WIFI_MGR_MODE_STA && rss_sources_available(settings)) {
        rss_active = rss_activate_next_source(settings);
    }

    // Fallback to custom messages when RSS is unavailable.
    if (!rss_active) {
        load_custom_or_prompt(settings, &current_msg,
                              "No messages     Press button to configure");
    }

    // Initialize BOOT button for config mode toggle.
    config_button_init();
    bool config_mode = false;

    ESP_LOGI(TAG, "ManCaveScroller ready - press BOOT for config mode");

    // Main loop owns display timing.
    while (1) {
        // BOOT button toggles config mode (STA only).
        if (config_button_pressed) {
            config_button_pressed = false;

            if (!config_mode && wifi_manager_get_mode() == WIFI_MGR_MODE_STA) {
                // Enter config mode: bring radio up and show IP.
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
                // Exit config mode: radio off, apply updated settings, then resume content flow.
                ESP_LOGI(TAG, "BOOT: exiting config mode");
                config_mode = false;
                web_server_stop();
                wifi_manager_radio_off();

                settings = settings_get();
                scroller_set_speed(settings->speed);
                led_panel_set_brightness(settings->brightness);
                led_panel_set_cols(settings->panel_cols);

                // Rebuild RSS scheduler state from latest settings.
                rss_active = false;
                rss_scheduler_reset();
                if (wifi_manager_get_mode() == WIFI_MGR_MODE_STA && rss_sources_available(settings)) {
                    rss_active = rss_activate_next_source(settings);
                }

                // Fall back to custom messages if no source can be activated.
                if (!rss_active) {
                    load_custom_or_prompt(settings, &current_msg,
                                          "No messages     Press button to configure");
                }
            }
        }

        bool cycle_done = false;
        int delay_ms = scroller_tick(&cycle_done);

        // Advance content only when current scroll cycle completes.
        if (cycle_done && !config_mode) {
            settings = settings_get();

            if (rss_active) {
                // Continue through current source items; when exhausted, switch sources.
                if (!rss_load_current_item()) {
                    if (!rss_activate_next_source(settings)) {
                        // No source currently ready/available; continue in custom mode until retry window.
                        rss_active = false;
                        load_custom_or_prompt(settings, &current_msg,
                                              "RSS unavailable     Press button to configure");
                    }
                }
            } else {
                // Custom-only mode: keep cycling enabled messages.
                int next = next_enabled_message(settings, current_msg);
                if (next >= 0) {
                    current_msg = next;
                    load_message(settings, current_msg);
                }

                // Periodically attempt to re-activate RSS sources (auto-recovery path).
                if (wifi_manager_get_mode() == WIFI_MGR_MODE_STA && rss_sources_available(settings)) {
                    TickType_t now = xTaskGetTickCount();
                    if ((int32_t)(now - rss_next_scheduler_tick) >= 0) {
                        if (rss_activate_next_source(settings)) {
                            rss_active = true;
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

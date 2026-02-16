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

// Load a custom message into the scroller by index
static void load_message(const app_settings_t *s, int idx)
{
    scroller_set_text(s->messages[idx].text);
    scroller_set_color(s->messages[idx].color_r,
                       s->messages[idx].color_g,
                       s->messages[idx].color_b);
}

// ── RSS color rotation ──

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

// ── RSS state ──

static int rss_item_idx = 0;
static bool rss_showing_title = true;
static int rss_items_since_custom = 0;
static bool rss_pending_custom = false;
static int rss_color_idx = 0;

// Fetch RSS feed: display status, turn WiFi on, fetch, turn WiFi off.
// Returns true if items were fetched successfully.
static bool fetch_rss_feed(const app_settings_t *s)
{
    ESP_LOGI(TAG, "Fetching RSS feed...");
    scroller_set_text("Updating news...");
    scroller_set_color(255, 255, 255);

    // Render one frame so "Updating news..." is visible while WiFi is active
    scroller_tick(NULL);

    bool connected = wifi_manager_radio_on();
    if (!connected) {
        ESP_LOGW(TAG, "WiFi connect failed for RSS fetch");
        wifi_manager_radio_off();
        return false;
    }

    esp_err_t err = rss_fetch(s->rss_url);
    wifi_manager_radio_off();

    if (err != ESP_OK || rss_get_count() == 0) {
        ESP_LOGW(TAG, "RSS fetch failed or empty");
        return false;
    }

    ESP_LOGI(TAG, "RSS: %d items ready", rss_get_count());
    return true;
}

// Load the next RSS scroll item (title or description) into the scroller.
// Handles interleaving with custom messages every 4 RSS items.
// Returns true if an item was loaded, false if RSS is exhausted and needs re-fetch.
static bool rss_advance(const app_settings_t *s, int *custom_msg_idx)
{
    // Custom message insertion point
    if (rss_pending_custom) {
        int next = next_enabled_message(s, *custom_msg_idx);
        if (next >= 0) {
            *custom_msg_idx = next;
            load_message(s, next);
        }
        rss_pending_custom = false;
        return true;
    }

    // Check if we've exhausted all RSS items
    if (rss_item_idx >= rss_get_count()) {
        return false;  // signal re-fetch needed
    }

    const rss_item_t *item = rss_get_item(rss_item_idx);
    if (!item) return false;

    const uint8_t *color = rss_colors[rss_color_idx % RSS_NUM_COLORS];

    if (rss_showing_title) {
        // Show title
        if (item->title[0] != '\0') {
            scroller_set_text(item->title);
        } else {
            scroller_set_text("(no title)");
        }
        scroller_set_color(color[0], color[1], color[2]);
        rss_showing_title = false;
    } else {
        // Show description
        if (item->description[0] != '\0') {
            scroller_set_text(item->description);
        } else {
            scroller_set_text("(no description)");
        }
        scroller_set_color(color[0], color[1], color[2]);

        // Advance to next item
        rss_item_idx++;
        rss_color_idx = (rss_color_idx + 1) % RSS_NUM_COLORS;
        rss_items_since_custom++;
        rss_showing_title = true;

        // Check if it's time for a custom message
        if (rss_items_since_custom >= 4) {
            if (next_enabled_message(s, *custom_msg_idx) >= 0) {
                rss_pending_custom = true;
            }
            rss_items_since_custom = 0;
        }
    }
    return true;
}

// Reset RSS playback state for a fresh feed cycle
static void rss_reset_playback(void)
{
    rss_item_idx = 0;
    rss_color_idx = 0;
    rss_showing_title = true;
    rss_items_since_custom = 0;
    rss_pending_custom = false;
    // Note: custom_msg_idx is NOT reset — it persists across feed cycles
}

void app_main(void)
{
    ESP_LOGI(TAG, "ManCaveScroller starting...");

    // Initialize NVS flash
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    // Load settings from NVS
    settings_init();
    app_settings_t *settings = settings_get();

    // Initialize LED panel
    ret = led_panel_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LED panel init failed!");
        return;
    }
    led_panel_set_brightness(settings->brightness);
    led_panel_set_cols(settings->panel_cols);

    // Initialize scroller state
    scroller_init();
    scroller_set_speed(settings->speed);

    // Start WiFi (blocks until connected or falls back to AP)
    wifi_manager_init();
    wifi_manager_start();
    web_server_start();

    // RSS active = STA mode + RSS enabled + URL configured
    bool rss_active = false;
    int custom_msg_idx = -1;  // persists across RSS feed cycles

    // Initial content setup
    int current_msg = -1;
    wifi_mgr_mode_t mode = wifi_manager_get_mode();
    ESP_LOGI(TAG, "WiFi mode=%d, rss_enabled=%d, rss_url='%.40s'",
             mode, settings->rss_enabled, settings->rss_url);

    if (mode == WIFI_MGR_MODE_STA) {
        // Try initial RSS fetch if enabled
        if (settings->rss_enabled && strlen(settings->rss_url) > 0) {
            if (fetch_rss_feed(settings)) {
                rss_active = true;
                rss_reset_playback();
                // Load the first RSS item
                rss_advance(settings, &custom_msg_idx);
            }
        }

        if (!rss_active) {
            // Fall back to custom messages
            current_msg = next_enabled_message(settings, MAX_MESSAGES - 1);
            if (current_msg >= 0) {
                load_message(settings, current_msg);
            } else {
                scroller_set_text("No messages     Press button to configure");
            }
        }
    } else {
        ESP_LOGI(TAG, "Not in STA mode — RSS requires STA, using custom messages");
    }

    // Initialize BOOT button for config mode toggle
    config_button_init();
    bool config_mode = false;

    ESP_LOGI(TAG, "ManCaveScroller ready — press BOOT for config mode");

    // Main loop owns the display
    while (1) {
        // Check for BOOT button press — toggle config mode (STA only)
        if (config_button_pressed) {
            config_button_pressed = false;

            if (!config_mode && wifi_manager_get_mode() == WIFI_MGR_MODE_STA) {
                // Enter config mode — bring WiFi back up
                ESP_LOGI(TAG, "BOOT: entering config mode");
                config_mode = true;
                if (wifi_manager_radio_on()) {
                    web_server_start();
                    char msg[64];
                    snprintf(msg, sizeof(msg), "Config Mode     %s",
                             wifi_manager_get_ip());
                    scroller_set_text(msg);
                } else {
                    scroller_set_text("Config Mode     WiFi failed");
                }
            } else if (config_mode) {
                // Exit config mode — turn WiFi off, apply any changes
                ESP_LOGI(TAG, "BOOT: exiting config mode");
                config_mode = false;
                web_server_stop();
                wifi_manager_radio_off();

                // Re-apply settings changed via web UI
                settings = settings_get();
                scroller_set_speed(settings->speed);
                led_panel_set_brightness(settings->brightness);
                led_panel_set_cols(settings->panel_cols);

                // Check if RSS should be (re-)activated
                rss_active = false;
                ESP_LOGI(TAG, "Config exit: rss_enabled=%d, rss_url='%.40s'",
                         settings->rss_enabled, settings->rss_url);
                if (settings->rss_enabled && strlen(settings->rss_url) > 0) {
                    if (fetch_rss_feed(settings)) {
                        rss_active = true;
                        rss_reset_playback();
                        rss_advance(settings, &custom_msg_idx);
                    }
                }

                if (!rss_active) {
                    // Fall back to custom messages
                    current_msg = next_enabled_message(settings, MAX_MESSAGES - 1);
                    if (current_msg >= 0) {
                        load_message(settings, current_msg);
                    } else {
                        scroller_set_text("No messages     Press button to configure");
                    }
                }
            }
        }

        bool cycle_done = false;
        int delay_ms = scroller_tick(&cycle_done);

        // Advance content when current scroll finishes
        if (cycle_done && !config_mode) {
            settings = settings_get();

            if (rss_active) {
                // RSS mode: advance through RSS items + custom interleaving
                if (!rss_advance(settings, &custom_msg_idx)) {
                    // All RSS items shown — re-fetch
                    if (fetch_rss_feed(settings)) {
                        rss_reset_playback();
                        rss_advance(settings, &custom_msg_idx);
                    } else {
                        // Fetch failed — fall back to custom only
                        rss_active = false;
                        current_msg = next_enabled_message(settings, MAX_MESSAGES - 1);
                        if (current_msg >= 0) {
                            load_message(settings, current_msg);
                        } else {
                            scroller_set_text("RSS unavailable     Press button to configure");
                        }
                    }
                }
            } else {
                // Custom-only mode: cycle through enabled messages
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

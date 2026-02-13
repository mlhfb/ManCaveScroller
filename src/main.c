#include <stdio.h>
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

    // Initialize scroller state
    scroller_init();
    scroller_set_color(settings->color_r, settings->color_g, settings->color_b);
    scroller_set_speed(settings->speed);

    // Start WiFi (blocks until connected or falls back to AP)
    wifi_manager_init();
    wifi_manager_start();
    web_server_start();

    // In STA mode, compose display text with IP; in AP mode, wifi_manager already set text
    if (wifi_manager_get_mode() == WIFI_MGR_MODE_STA) {
        char display_text[SCROLLER_MAX_TEXT_LEN + 64];
        snprintf(display_text, sizeof(display_text),
                 "%s     Press button to enter configuration", settings->text);
        scroller_set_text(display_text);
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
                    char msg[SCROLLER_MAX_TEXT_LEN + 32];
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
                wifi_manager_radio_off();

                // Re-apply settings changed via web UI
                settings = settings_get();
                scroller_set_color(settings->color_r, settings->color_g, settings->color_b);
                scroller_set_speed(settings->speed);
                led_panel_set_brightness(settings->brightness);

                char display_text[SCROLLER_MAX_TEXT_LEN + 64];
                snprintf(display_text, sizeof(display_text),
                         "%s     Press button to enter configuration", settings->text);
                scroller_set_text(display_text);
            }
        }

        bool cycle_done = false;
        int delay_ms = scroller_tick(&cycle_done);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

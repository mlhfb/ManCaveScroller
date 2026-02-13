#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "led_panel.h"
#include "settings.h"
#include "text_scroller.h"
#include "wifi_manager.h"
#include "web_server.h"

static const char *TAG = "main";

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
        char display_text[SCROLLER_MAX_TEXT_LEN + 32];
        snprintf(display_text, sizeof(display_text), "%s     connect at %s",
                 settings->text, wifi_manager_get_ip());
        scroller_set_text(display_text);
    }

    ESP_LOGI(TAG, "ManCaveScroller ready — entering display loop");

    // Main loop owns the display — like a vintage computer driving the CRT
    while (1) {
        bool cycle_done = false;
        int delay_ms = scroller_tick(&cycle_done);

        if (cycle_done && wifi_manager_get_mode() == WIFI_MGR_MODE_STA) {
            // WiFi service window between message cycles
            wifi_manager_radio_on();  // reconnect (up to 5s) + serve requests (2s)

            // Re-apply any settings changed via web UI
            settings = settings_get();
            scroller_set_color(settings->color_r, settings->color_g, settings->color_b);
            scroller_set_speed(settings->speed);
            led_panel_set_brightness(settings->brightness);

            // Compose display text with latest message + IP
            char display_text[SCROLLER_MAX_TEXT_LEN + 32];
            snprintf(display_text, sizeof(display_text), "%s     connect at %s",
                     settings->text, wifi_manager_get_ip());
            scroller_set_text(display_text);

            wifi_manager_radio_off();
        }

        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

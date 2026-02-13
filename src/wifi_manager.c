#include "wifi_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "settings.h"
#include "text_scroller.h"

static const char *TAG = "wifi_mgr";

#define AP_SSID         "ManCave"
#define AP_MAX_CONN     4
#define STA_MAX_RETRY   5
#define STA_CONNECT_TIMEOUT_MS 15000

static wifi_mgr_mode_t current_mode = WIFI_MGR_MODE_NONE;
static char current_ip[16] = "0.0.0.0";
static char current_ssid[33] = "";
static int sta_retry_count = 0;
static bool radio_cycling = false;
static EventGroupHandle_t wifi_event_group;
static TaskHandle_t dns_task_handle = NULL;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static void start_ap_mode(void);
static void start_sta_mode(const char *ssid, const char *password);
static void dns_server_task(void *param);

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (radio_cycling) {
                // During radio cycling, allow one retry then give up
                if (sta_retry_count < 1) {
                    sta_retry_count++;
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            }
            if (sta_retry_count < STA_MAX_RETRY) {
                sta_retry_count++;
                ESP_LOGI(TAG, "Retrying STA connection (%d/%d)", sta_retry_count, STA_MAX_RETRY);
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "STA connection failed after %d retries, falling back to AP", STA_MAX_RETRY);
                xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
            ESP_LOGI(TAG, "Client connected to AP, AID=%d", event->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
            ESP_LOGI(TAG, "Client disconnected from AP, AID=%d", event->aid);
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "STA connected, IP: %s", current_ip);
        current_mode = WIFI_MGR_MODE_STA;
        sta_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP mode: SSID=%s", AP_SSID);

    // Stop any existing WiFi
    esp_wifi_stop();

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = AP_SSID,
            .ssid_len = strlen(AP_SSID),
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    strcpy(current_ip, "192.168.4.1");
    strcpy(current_ssid, AP_SSID);
    current_mode = WIFI_MGR_MODE_AP;

    // Start captive portal DNS server
    if (dns_task_handle == NULL) {
        xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 3, &dns_task_handle);
    }

    scroller_set_text("connect to ManCave");

    ESP_LOGI(TAG, "AP mode active at %s", current_ip);
}

static void start_sta_mode(const char *ssid, const char *password)
{
    ESP_LOGI(TAG, "Starting STA mode, connecting to: %s", ssid);

    // Stop DNS server if running
    if (dns_task_handle != NULL) {
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }

    esp_wifi_stop();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);

    sta_retry_count = 0;
    current_mode = WIFI_MGR_MODE_STA_CONNECTING;
    strncpy(current_ssid, ssid, sizeof(current_ssid) - 1);

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    // Wait for connection or failure
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "STA connected to %s — suspending WiFi for display", ssid);
        esp_wifi_stop();
    } else {
        ESP_LOGW(TAG, "STA connection to %s failed, starting AP mode", ssid);
        start_ap_mode();
    }
}

// Minimal DNS server for captive portal.
// Responds to all DNS queries with 192.168.4.1.
static void dns_server_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "DNS captive portal server started");

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx_buffer[128];
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len < 12) continue; // DNS header is 12 bytes minimum

        // Build DNS response
        uint8_t response[256];
        memcpy(response, rx_buffer, len); // copy the query

        // Set response flags: QR=1 (response), AA=1 (authoritative)
        response[2] = 0x84; // QR=1, Opcode=0, AA=1
        response[3] = 0x00; // No error
        // Set answer count to 1
        response[6] = 0x00;
        response[7] = 0x01;

        // Append answer: pointer to query name + A record with 192.168.4.1
        int resp_len = len;
        response[resp_len++] = 0xC0; // Name pointer
        response[resp_len++] = 0x0C; // Offset to question name
        response[resp_len++] = 0x00; // Type A
        response[resp_len++] = 0x01;
        response[resp_len++] = 0x00; // Class IN
        response[resp_len++] = 0x01;
        response[resp_len++] = 0x00; // TTL = 60 seconds
        response[resp_len++] = 0x00;
        response[resp_len++] = 0x00;
        response[resp_len++] = 0x3C;
        response[resp_len++] = 0x00; // Data length = 4
        response[resp_len++] = 0x04;
        response[resp_len++] = 192;  // 192.168.4.1
        response[resp_len++] = 168;
        response[resp_len++] = 4;
        response[resp_len++] = 1;

        sendto(sock, response, resp_len, 0,
               (struct sockaddr *)&client_addr, addr_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

void wifi_manager_init(void)
{
    wifi_event_group = xEventGroupCreate();

    esp_netif_init();
    esp_event_loop_create_default();

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);
}

void wifi_manager_start(void)
{
    app_settings_t *settings = settings_get();

    if (strlen(settings->wifi_ssid) > 0) {
        start_sta_mode(settings->wifi_ssid, settings->wifi_password);
    } else {
        start_ap_mode();
    }
}

wifi_mgr_mode_t wifi_manager_get_mode(void)
{
    return current_mode;
}

const char *wifi_manager_get_ip(void)
{
    return current_ip;
}

const char *wifi_manager_get_ssid(void)
{
    return current_ssid;
}

void wifi_manager_set_sta_credentials(const char *ssid, const char *password)
{
    app_settings_t *settings = settings_get();
    strncpy(settings->wifi_ssid, ssid, SETTINGS_MAX_SSID_LEN);
    settings->wifi_ssid[SETTINGS_MAX_SSID_LEN] = '\0';
    strncpy(settings->wifi_password, password, SETTINGS_MAX_PASS_LEN);
    settings->wifi_password[SETTINGS_MAX_PASS_LEN] = '\0';
    settings_save(settings);

    // Attempt STA connection with new credentials
    start_sta_mode(ssid, password);
}

void wifi_manager_radio_on(void)
{
    if (current_mode != WIFI_MGR_MODE_STA) return;
    radio_cycling = true;
    sta_retry_count = 0;
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    esp_wifi_start();

    // Wait up to 5 seconds for reconnection
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(5000));
    if (bits & WIFI_CONNECTED_BIT) {
        // Connected — serve web requests for 2 seconds
        ESP_LOGI(TAG, "Radio on — serving requests for 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void wifi_manager_radio_off(void)
{
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(50)); // let pending disconnect events drain
    radio_cycling = false;
}

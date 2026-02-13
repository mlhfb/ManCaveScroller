#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    WIFI_MGR_MODE_NONE,
    WIFI_MGR_MODE_AP,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_STA_CONNECTING
} wifi_mgr_mode_t;

void wifi_manager_init(void);
void wifi_manager_start(void);
wifi_mgr_mode_t wifi_manager_get_mode(void);
const char *wifi_manager_get_ip(void);
const char *wifi_manager_get_ssid(void);
void wifi_manager_set_sta_credentials(const char *ssid, const char *password);
bool wifi_manager_radio_on(void);   // re-enable WiFi radio (STA mode only), returns true if connected
void wifi_manager_radio_off(void);  // disable WiFi radio

#endif

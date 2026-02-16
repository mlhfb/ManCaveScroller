#include "web_server.h"
#include "web_page.h"
#include "text_scroller.h"
#include "settings.h"
#include "wifi_manager.h"
#include "led_panel.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "cJSON.h"

static const char *TAG = "web_server";
static httpd_handle_t server = NULL;

// GET / — serve the web page
static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, WEB_PAGE, strlen(WEB_PAGE));
    return ESP_OK;
}

// GET /api/status — return current settings as JSON
static esp_err_t status_handler(httpd_req_t *req)
{
    app_settings_t *s = settings_get();
    const char *mode_str;
    switch (wifi_manager_get_mode()) {
    case WIFI_MGR_MODE_AP:             mode_str = "AP"; break;
    case WIFI_MGR_MODE_STA:            mode_str = "STA"; break;
    case WIFI_MGR_MODE_STA_CONNECTING: mode_str = "Connecting"; break;
    default:                           mode_str = "None"; break;
    }

    cJSON *root = cJSON_CreateObject();

    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    for (int i = 0; i < MAX_MESSAGES; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "text", s->messages[i].text);
        cJSON_AddNumberToObject(m, "r", s->messages[i].color_r);
        cJSON_AddNumberToObject(m, "g", s->messages[i].color_g);
        cJSON_AddNumberToObject(m, "b", s->messages[i].color_b);
        cJSON_AddBoolToObject(m, "enabled", s->messages[i].enabled);
        cJSON_AddItemToArray(msgs, m);
    }

    cJSON_AddNumberToObject(root, "speed", s->speed);
    cJSON_AddNumberToObject(root, "brightness", s->brightness);
    cJSON_AddStringToObject(root, "wifi_mode", mode_str);
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    cJSON_AddNumberToObject(root, "panel_cols", s->panel_cols);
    cJSON_AddStringToObject(root, "wifi_ssid", s->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", s->wifi_password);
    cJSON_AddBoolToObject(root, "rss_enabled", s->rss_enabled);
    cJSON_AddStringToObject(root, "rss_url", s->rss_url);
    cJSON_AddNumberToObject(root, "rss_source_count", s->rss_source_count);
    cJSON *rss_sources = cJSON_AddArrayToObject(root, "rss_sources");
    int source_count = s->rss_source_count;
    if (source_count < 1 || source_count > MAX_RSS_SOURCES) source_count = 1;
    for (int i = 0; i < source_count; i++) {
        cJSON *src = cJSON_CreateObject();
        cJSON_AddStringToObject(src, "name", s->rss_sources[i].name);
        cJSON_AddBoolToObject(src, "enabled", s->rss_sources[i].enabled);
        cJSON_AddStringToObject(src, "url", s->rss_sources[i].url);
        cJSON_AddItemToArray(rss_sources, src);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    cJSON_free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// Helper: read POST body and parse as JSON
static cJSON *read_json_body(httpd_req_t *req)
{
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 4096) return NULL;

    char *buf = malloc(total_len + 1);
    if (!buf) return NULL;

    int received = httpd_req_recv(req, buf, total_len);
    if (received <= 0) { free(buf); return NULL; }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

static void send_ok(httpd_req_t *req, const char *msg)
{
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"status\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
}

static void send_err(httpd_req_t *req, const char *msg)
{
    char resp[128];
    snprintf(resp, sizeof(resp), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, resp, strlen(resp));
}

// POST /api/messages — update all messages
static esp_err_t messages_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    cJSON *msgs = cJSON_GetObjectItem(json, "messages");
    if (!cJSON_IsArray(msgs)) {
        cJSON_Delete(json);
        send_err(req, "Missing 'messages' array");
        return ESP_OK;
    }

    app_settings_t *s = settings_get();
    int count = cJSON_GetArraySize(msgs);
    if (count > MAX_MESSAGES) count = MAX_MESSAGES;

    for (int i = 0; i < count; i++) {
        cJSON *m = cJSON_GetArrayItem(msgs, i);
        if (!cJSON_IsObject(m)) continue;

        cJSON *text = cJSON_GetObjectItem(m, "text");
        if (cJSON_IsString(text)) {
            strncpy(s->messages[i].text, text->valuestring, SETTINGS_MAX_TEXT_LEN);
            s->messages[i].text[SETTINGS_MAX_TEXT_LEN] = '\0';
        }

        cJSON *r = cJSON_GetObjectItem(m, "r");
        cJSON *g = cJSON_GetObjectItem(m, "g");
        cJSON *b = cJSON_GetObjectItem(m, "b");
        if (cJSON_IsNumber(r)) s->messages[i].color_r = (uint8_t)r->valueint;
        if (cJSON_IsNumber(g)) s->messages[i].color_g = (uint8_t)g->valueint;
        if (cJSON_IsNumber(b)) s->messages[i].color_b = (uint8_t)b->valueint;

        cJSON *en = cJSON_GetObjectItem(m, "enabled");
        if (cJSON_IsBool(en)) s->messages[i].enabled = cJSON_IsTrue(en);
    }

    settings_save(s);
    cJSON_Delete(json);
    send_ok(req, "Messages updated");
    return ESP_OK;
}

// POST /api/text — update message[0] text (legacy)
static esp_err_t text_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    cJSON *text = cJSON_GetObjectItem(json, "text");
    if (!cJSON_IsString(text)) {
        cJSON_Delete(json);
        send_err(req, "Missing 'text' field");
        return ESP_OK;
    }

    app_settings_t *s = settings_get();
    strncpy(s->messages[0].text, text->valuestring, SETTINGS_MAX_TEXT_LEN);
    s->messages[0].text[SETTINGS_MAX_TEXT_LEN] = '\0';
    s->messages[0].enabled = true;
    scroller_set_text(s->messages[0].text);
    settings_save(s);

    cJSON_Delete(json);
    send_ok(req, "Text updated");
    return ESP_OK;
}

// POST /api/color — update message[0] color (legacy)
static esp_err_t color_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    cJSON *r = cJSON_GetObjectItem(json, "r");
    cJSON *g = cJSON_GetObjectItem(json, "g");
    cJSON *b = cJSON_GetObjectItem(json, "b");
    if (!cJSON_IsNumber(r) || !cJSON_IsNumber(g) || !cJSON_IsNumber(b)) {
        cJSON_Delete(json);
        send_err(req, "Missing r/g/b fields");
        return ESP_OK;
    }

    app_settings_t *s = settings_get();
    s->messages[0].color_r = (uint8_t)r->valueint;
    s->messages[0].color_g = (uint8_t)g->valueint;
    s->messages[0].color_b = (uint8_t)b->valueint;
    scroller_set_color(s->messages[0].color_r, s->messages[0].color_g, s->messages[0].color_b);
    settings_save(s);

    cJSON_Delete(json);
    send_ok(req, "Color updated");
    return ESP_OK;
}

// POST /api/speed — update scroll speed
static esp_err_t speed_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    cJSON *speed = cJSON_GetObjectItem(json, "speed");
    if (!cJSON_IsNumber(speed)) {
        cJSON_Delete(json);
        send_err(req, "Missing 'speed' field");
        return ESP_OK;
    }

    app_settings_t *s = settings_get();
    s->speed = (uint8_t)speed->valueint;
    scroller_set_speed(s->speed);
    settings_save(s);

    cJSON_Delete(json);
    send_ok(req, "Speed updated");
    return ESP_OK;
}

// POST /api/brightness — update brightness
static esp_err_t brightness_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    cJSON *bright = cJSON_GetObjectItem(json, "brightness");
    if (!cJSON_IsNumber(bright)) {
        cJSON_Delete(json);
        send_err(req, "Missing 'brightness' field");
        return ESP_OK;
    }

    app_settings_t *s = settings_get();
    s->brightness = (uint8_t)bright->valueint;
    led_panel_set_brightness(s->brightness);
    settings_save(s);

    cJSON_Delete(json);
    send_ok(req, "Brightness updated");
    return ESP_OK;
}

// POST /api/wifi — set WiFi credentials and attempt connection
static esp_err_t wifi_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *password = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid)) {
        cJSON_Delete(json);
        send_err(req, "Missing 'ssid' field");
        return ESP_OK;
    }

    const char *pass_str = cJSON_IsString(password) ? password->valuestring : "";

    // Send response before attempting connection (connection will change network)
    send_ok(req, "Connecting to WiFi...");

    wifi_manager_set_sta_credentials(ssid->valuestring, pass_str);

    cJSON_Delete(json);
    return ESP_OK;
}

// POST /api/appearance — update speed and brightness together
static esp_err_t appearance_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    app_settings_t *s = settings_get();

    cJSON *speed = cJSON_GetObjectItem(json, "speed");
    if (cJSON_IsNumber(speed)) {
        s->speed = (uint8_t)speed->valueint;
        scroller_set_speed(s->speed);
    }

    cJSON *bright = cJSON_GetObjectItem(json, "brightness");
    if (cJSON_IsNumber(bright)) {
        s->brightness = (uint8_t)bright->valueint;
        led_panel_set_brightness(s->brightness);
    }

    settings_save(s);
    cJSON_Delete(json);
    send_ok(req, "Appearance updated");
    return ESP_OK;
}

// POST /api/advanced — update advanced settings (panel_cols)
static esp_err_t advanced_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    app_settings_t *s = settings_get();

    cJSON *cols = cJSON_GetObjectItem(json, "panel_cols");
    if (cJSON_IsNumber(cols)) {
        uint8_t val = (uint8_t)cols->valueint;
        if (val == 32 || val == 64 || val == 96 || val == 128) {
            s->panel_cols = val;
            led_panel_set_cols(val);
        }
    }

    settings_save(s);
    cJSON_Delete(json);
    send_ok(req, "Advanced settings updated");
    return ESP_OK;
}

// POST /api/rss — update RSS settings
static esp_err_t rss_handler(httpd_req_t *req)
{
    cJSON *json = read_json_body(req);
    if (!json) { send_err(req, "Invalid JSON"); return ESP_OK; }

    app_settings_t *s = settings_get();

    cJSON *en = cJSON_GetObjectItem(json, "enabled");
    if (cJSON_IsBool(en)) s->rss_enabled = cJSON_IsTrue(en);

    cJSON *url = cJSON_GetObjectItem(json, "url");
    if (cJSON_IsString(url)) {
        strncpy(s->rss_url, url->valuestring, SETTINGS_MAX_URL_LEN);
        s->rss_url[SETTINGS_MAX_URL_LEN] = '\0';
    }
    // Legacy endpoint controls source slot 0.
    s->rss_source_count = 1;
    s->rss_sources[0].enabled = s->rss_enabled;
    if (s->rss_sources[0].name[0] == '\0') {
        strncpy(s->rss_sources[0].name, "Primary RSS", SETTINGS_MAX_RSS_NAME_LEN);
        s->rss_sources[0].name[SETTINGS_MAX_RSS_NAME_LEN] = '\0';
    }
    strncpy(s->rss_sources[0].url, s->rss_url, SETTINGS_MAX_URL_LEN);
    s->rss_sources[0].url[SETTINGS_MAX_URL_LEN] = '\0';

    ESP_LOGI(TAG, "RSS save: enabled=%d, url='%.60s'", s->rss_enabled, s->rss_url);
    settings_save(s);
    cJSON_Delete(json);
    send_ok(req, "RSS settings updated");
    return ESP_OK;
}

// POST /api/factory-reset — erase NVS and restart
static esp_err_t factory_reset_handler(httpd_req_t *req)
{
    send_ok(req, "Factory reset — restarting...");
    vTaskDelay(pdMS_TO_TICKS(500));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;  // unreachable
}

// Captive portal: redirect all unknown URIs to /
static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void web_server_start(void)
{
    if (server != NULL) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    // Register URI handlers (order matters for wildcard matching)
    httpd_uri_t uris[] = {
        {.uri = "/",               .method = HTTP_GET,  .handler = root_handler},
        {.uri = "/api/status",     .method = HTTP_GET,  .handler = status_handler},
        {.uri = "/api/messages",   .method = HTTP_POST, .handler = messages_handler},
        {.uri = "/api/text",       .method = HTTP_POST, .handler = text_handler},
        {.uri = "/api/color",      .method = HTTP_POST, .handler = color_handler},
        {.uri = "/api/speed",      .method = HTTP_POST, .handler = speed_handler},
        {.uri = "/api/brightness", .method = HTTP_POST, .handler = brightness_handler},
        {.uri = "/api/wifi",          .method = HTTP_POST, .handler = wifi_handler},
        {.uri = "/api/appearance",    .method = HTTP_POST, .handler = appearance_handler},
        {.uri = "/api/advanced",      .method = HTTP_POST, .handler = advanced_handler},
        {.uri = "/api/rss",           .method = HTTP_POST, .handler = rss_handler},
        {.uri = "/api/factory-reset", .method = HTTP_POST, .handler = factory_reset_handler},
        {.uri = "/*",                 .method = HTTP_GET,  .handler = captive_redirect_handler},
    };

    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }

    ESP_LOGI(TAG, "Web server started");
}

void web_server_stop(void)
{
    if (server != NULL) {
        httpd_stop(server);
        server = NULL;
    }
}

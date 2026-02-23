#include "settings.h"
#include "storage_paths.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "settings";
static const char *NVS_NAMESPACE = "mancave";

static app_settings_t current_settings;

static bool ends_with_php(const char *s)
{
    if (!s) return false;
    size_t len = strlen(s);
    if (len < 4) return false;
    return (tolower((unsigned char)s[len - 4]) == '.' &&
            tolower((unsigned char)s[len - 3]) == 'p' &&
            tolower((unsigned char)s[len - 2]) == 'h' &&
            tolower((unsigned char)s[len - 1]) == 'p');
}

static bool base_has_php_script(const char *base_url)
{
    if (!base_url || base_url[0] == '\0') return false;

    char work[SETTINGS_MAX_URL_LEN + 1] = {0};
    strncpy(work, base_url, SETTINGS_MAX_URL_LEN);
    work[SETTINGS_MAX_URL_LEN] = '\0';

    char *query = strchr(work, '?');
    if (query) *query = '\0';
    char *fragment = strchr(work, '#');
    if (fragment) *fragment = '\0';

    size_t len = strlen(work);
    while (len > 0 && work[len - 1] == '/') {
        work[--len] = '\0';
    }
    if (len == 0) return false;

    const char *last_slash = strrchr(work, '/');
    const char *leaf = last_slash ? (last_slash + 1) : work;
    return ends_with_php(leaf);
}

static void trim_ascii(char *s)
{
    if (!s || s[0] == '\0') return;

    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void normalize_sports_base_url(const char *raw, char *out, size_t out_size)
{
    char local[SETTINGS_MAX_URL_LEN + 1] = {0};

    if (raw) {
        strncpy(local, raw, SETTINGS_MAX_URL_LEN);
        local[SETTINGS_MAX_URL_LEN] = '\0';
    }
    trim_ascii(local);

    if (local[0] == '\0') {
        out[0] = '\0';
        return;
    }

    if (strstr(local, "://") == NULL) {
        snprintf(out, out_size, "https://%s", local);
    } else {
        strncpy(out, local, out_size - 1);
        out[out_size - 1] = '\0';
    }

    bool has_php_path = base_has_php_script(out);
    size_t len = strlen(out);
    if (!has_php_path && len > 0 && out[len - 1] != '/' && len < out_size - 1) {
        out[len] = '/';
        out[len + 1] = '\0';
    }
}

static void build_espn_feed_url(const char *base_url, const char *sport_code, char *out, size_t out_size)
{
    if (base_has_php_script(base_url)) {
        const char *sep = strchr(base_url, '?') ? "&" : "?";
        snprintf(out, out_size, "%s%ssport=%s&format=rss", base_url, sep, sport_code);
        return;
    }
    snprintf(out, out_size, "%sespn_scores_rss.php?sport=%s&format=rss", base_url, sport_code);
}

static void add_rss_source(app_settings_t *s, const char *name, const char *url)
{
    if (!s || !name || !url || url[0] == '\0' || s->rss_source_count >= MAX_RSS_SOURCES) {
        return;
    }

    int idx = s->rss_source_count++;
    s->rss_sources[idx].enabled = true;
    strncpy(s->rss_sources[idx].name, name, SETTINGS_MAX_RSS_NAME_LEN);
    s->rss_sources[idx].name[SETTINGS_MAX_RSS_NAME_LEN] = '\0';
    strncpy(s->rss_sources[idx].url, url, SETTINGS_MAX_URL_LEN);
    s->rss_sources[idx].url[SETTINGS_MAX_URL_LEN] = '\0';
}

static void rebuild_rss_sources(app_settings_t *s)
{
    if (!s) return;

    trim_ascii(s->rss_url);

    char normalized_base[SETTINGS_MAX_URL_LEN + 1] = {0};
    normalize_sports_base_url(s->rss_sports_base_url, normalized_base, sizeof(normalized_base));
    strncpy(s->rss_sports_base_url, normalized_base, SETTINGS_MAX_URL_LEN);
    s->rss_sports_base_url[SETTINGS_MAX_URL_LEN] = '\0';

    memset(s->rss_sources, 0, sizeof(s->rss_sources));
    s->rss_source_count = 0;

    if (!s->rss_enabled) {
        return;
    }

    // Feed manifest order is fixed so display/caching logic can rely on stable indices:
    // MLB, NHL, NCAAF, NFL, NBA, BIG10, NPR.
    if (s->rss_sports_enabled && normalized_base[0] != '\0') {
        char sport_url[SETTINGS_MAX_URL_LEN + 1] = {0};

        if (s->rss_sport_mlb_enabled) {
            build_espn_feed_url(normalized_base, "mlb", sport_url, sizeof(sport_url));
            add_rss_source(s, "MLB Scores", sport_url);
        }
        if (s->rss_sport_nhl_enabled) {
            build_espn_feed_url(normalized_base, "nhl", sport_url, sizeof(sport_url));
            add_rss_source(s, "NHL Scores", sport_url);
        }
        if (s->rss_sport_ncaaf_enabled) {
            build_espn_feed_url(normalized_base, "ncaaf", sport_url, sizeof(sport_url));
            add_rss_source(s, "NCAAF Scores", sport_url);
        }
        if (s->rss_sport_nfl_enabled) {
            build_espn_feed_url(normalized_base, "nfl", sport_url, sizeof(sport_url));
            add_rss_source(s, "NFL Scores", sport_url);
        }
        if (s->rss_sport_nba_enabled) {
            build_espn_feed_url(normalized_base, "nba", sport_url, sizeof(sport_url));
            add_rss_source(s, "NBA Scores", sport_url);
        }
        if (s->rss_sport_big10_enabled) {
            build_espn_feed_url(normalized_base, "big10", sport_url, sizeof(sport_url));
            add_rss_source(s, "Big 10 Scores", sport_url);
        }
    }

    if (s->rss_npr_enabled && s->rss_url[0] != '\0') {
        add_rss_source(s, "NPR News", s->rss_url);
    }
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static void set_factory_defaults(app_settings_t *s)
{
    static const uint8_t default_colors[MAX_MESSAGES][3] = {
        {255, 0, 0},
        {0, 255, 0},
        {0, 0, 255},
        {255, 255, 0},
        {255, 0, 255},
    };

    memset(s, 0, sizeof(*s));

    for (int i = 0; i < MAX_MESSAGES; i++) {
        s->messages[i].color_r = default_colors[i][0];
        s->messages[i].color_g = default_colors[i][1];
        s->messages[i].color_b = default_colors[i][2];
    }

    s->speed = 5;
    s->brightness = 32;
    s->panel_cols = 32;
    s->rss_enabled = true;
    strncpy(s->rss_url, "https://feeds.npr.org/1001/rss.xml", SETTINGS_MAX_URL_LEN);
    s->rss_url[SETTINGS_MAX_URL_LEN] = '\0';
    s->rss_npr_enabled = true;
    s->rss_sports_enabled = false;
    s->rss_sports_base_url[0] = '\0';
    s->rss_sport_mlb_enabled = true;
    s->rss_sport_nhl_enabled = true;
    s->rss_sport_ncaaf_enabled = true;
    s->rss_sport_nfl_enabled = true;
    s->rss_sport_nba_enabled = true;
    s->rss_sport_big10_enabled = true;
    rebuild_rss_sources(s);
}

static bool load_default_messages_from_littlefs(app_settings_t *s)
{
    FILE *fp = fopen(LITTLEFS_DEFAULT_MESSAGES_PATH, "rb");
    if (!fp) {
        ESP_LOGW(TAG, "Default message file missing: %s", LITTLEFS_DEFAULT_MESSAGES_PATH);
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long size = ftell(fp);
    if (size <= 0 || size > 8192) {
        fclose(fp);
        ESP_LOGW(TAG, "Default message file size invalid: %ld", size);
        return false;
    }
    rewind(fp);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return false;
    }

    size_t read = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    buf[read] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse default messages JSON");
        return false;
    }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (!cJSON_IsArray(messages)) {
        cJSON_Delete(root);
        return false;
    }

    int count = cJSON_GetArraySize(messages);
    if (count > MAX_MESSAGES) count = MAX_MESSAGES;

    bool any_message = false;
    for (int i = 0; i < count; i++) {
        cJSON *entry = cJSON_GetArrayItem(messages, i);
        if (!cJSON_IsObject(entry)) continue;

        cJSON *text = cJSON_GetObjectItem(entry, "text");
        if (cJSON_IsString(text)) {
            strncpy(s->messages[i].text, text->valuestring, SETTINGS_MAX_TEXT_LEN);
            s->messages[i].text[SETTINGS_MAX_TEXT_LEN] = '\0';
        }

        cJSON *r = cJSON_GetObjectItem(entry, "r");
        cJSON *g = cJSON_GetObjectItem(entry, "g");
        cJSON *b = cJSON_GetObjectItem(entry, "b");
        if (cJSON_IsNumber(r)) s->messages[i].color_r = clamp_u8(r->valueint);
        if (cJSON_IsNumber(g)) s->messages[i].color_g = clamp_u8(g->valueint);
        if (cJSON_IsNumber(b)) s->messages[i].color_b = clamp_u8(b->valueint);

        cJSON *enabled = cJSON_GetObjectItem(entry, "enabled");
        if (cJSON_IsBool(enabled)) {
            s->messages[i].enabled = cJSON_IsTrue(enabled);
        }

        if (s->messages[i].enabled && s->messages[i].text[0] != '\0') {
            any_message = true;
        }
    }

    cJSON_Delete(root);
    return any_message;
}

static void load_default_settings(app_settings_t *s)
{
    set_factory_defaults(s);

    if (!load_default_messages_from_littlefs(s)) {
        strncpy(s->messages[0].text, "Hello Man Cave!", SETTINGS_MAX_TEXT_LEN);
        s->messages[0].text[SETTINGS_MAX_TEXT_LEN] = '\0';
        s->messages[0].enabled = true;
    }

    rebuild_rss_sources(s);
}

static void load_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved settings, using defaults");
        load_default_settings(&current_settings);
        return;
    }

    // Start with defaults, then override with stored values.
    load_default_settings(&current_settings);

    // Migrate old single-message format if present.
    char old_text[SETTINGS_MAX_TEXT_LEN + 1] = "";
    size_t len = sizeof(old_text);
    if (nvs_get_str(handle, "text", old_text, &len) == ESP_OK && strlen(old_text) > 0) {
        ESP_LOGI(TAG, "Migrating old single-message to messages[0]");
        strncpy(current_settings.messages[0].text, old_text, SETTINGS_MAX_TEXT_LEN);
        current_settings.messages[0].enabled = true;
        nvs_get_u8(handle, "color_r", &current_settings.messages[0].color_r);
        nvs_get_u8(handle, "color_g", &current_settings.messages[0].color_g);
        nvs_get_u8(handle, "color_b", &current_settings.messages[0].color_b);
        nvs_erase_key(handle, "text");
        nvs_erase_key(handle, "color_r");
        nvs_erase_key(handle, "color_g");
        nvs_erase_key(handle, "color_b");
        nvs_commit(handle);
    }

    // Load messages array.
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

    uint8_t rss_npr_en = current_settings.rss_npr_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_npr_en", &rss_npr_en);
    current_settings.rss_npr_enabled = (rss_npr_en != 0);

    uint8_t rss_sports_en = current_settings.rss_sports_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_sports_en", &rss_sports_en);
    current_settings.rss_sports_enabled = (rss_sports_en != 0);

    len = sizeof(current_settings.rss_sports_base_url);
    nvs_get_str(handle, "rss_sports_base", current_settings.rss_sports_base_url, &len);

    uint8_t sport_en = current_settings.rss_sport_mlb_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_mlb_en", &sport_en);
    current_settings.rss_sport_mlb_enabled = (sport_en != 0);

    sport_en = current_settings.rss_sport_nhl_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_nhl_en", &sport_en);
    current_settings.rss_sport_nhl_enabled = (sport_en != 0);

    sport_en = current_settings.rss_sport_ncaaf_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_ncaaf_en", &sport_en);
    current_settings.rss_sport_ncaaf_enabled = (sport_en != 0);

    sport_en = current_settings.rss_sport_nfl_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_nfl_en", &sport_en);
    current_settings.rss_sport_nfl_enabled = (sport_en != 0);

    sport_en = current_settings.rss_sport_nba_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_nba_en", &sport_en);
    current_settings.rss_sport_nba_enabled = (sport_en != 0);

    sport_en = current_settings.rss_sport_big10_enabled ? 1 : 0;
    nvs_get_u8(handle, "rss_big10_en", &sport_en);
    current_settings.rss_sport_big10_enabled = (sport_en != 0);

    rebuild_rss_sources(&current_settings);

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
    rebuild_rss_sources(&current_settings);

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
    nvs_set_u8(handle, "rss_npr_en", current_settings.rss_npr_enabled ? 1 : 0);
    nvs_set_u8(handle, "rss_sports_en", current_settings.rss_sports_enabled ? 1 : 0);
    nvs_set_str(handle, "rss_sports_base", current_settings.rss_sports_base_url);
    nvs_set_u8(handle, "rss_mlb_en", current_settings.rss_sport_mlb_enabled ? 1 : 0);
    nvs_set_u8(handle, "rss_nhl_en", current_settings.rss_sport_nhl_enabled ? 1 : 0);
    nvs_set_u8(handle, "rss_ncaaf_en", current_settings.rss_sport_ncaaf_enabled ? 1 : 0);
    nvs_set_u8(handle, "rss_nfl_en", current_settings.rss_sport_nfl_enabled ? 1 : 0);
    nvs_set_u8(handle, "rss_nba_en", current_settings.rss_sport_nba_enabled ? 1 : 0);
    nvs_set_u8(handle, "rss_big10_en", current_settings.rss_sport_big10_enabled ? 1 : 0);
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

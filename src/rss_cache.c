#include "rss_cache.h"
#include "storage_paths.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "rss_cache";

#define RSS_CACHE_DIR LITTLEFS_BASE_PATH "/cache"
#define RSS_CACHE_MAGIC 0x52434348u  // "RCCH"
#define RSS_CACHE_VERSION 1u
#define RSS_CACHE_MAX_SOURCES 16

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t item_count;
    uint32_t updated_epoch;
} rss_cache_header_t;

typedef struct {
    char title[RSS_TITLE_LEN + 1];
    char description[RSS_DESC_LEN + 1];
} rss_cache_record_t;

typedef struct {
    uint32_t item_count;
    uint32_t shown_count;
    uint8_t *shown_bits;
} cycle_source_state_t;

typedef struct {
    bool valid;
    uint32_t signature;
    int source_count;
    uint32_t total_items;
    uint32_t remaining_items;
    cycle_source_state_t sources[RSS_CACHE_MAX_SOURCES];
} cycle_state_t;

static cycle_state_t g_cycle_state = {0};

static uint32_t hash_url(const char *s)
{
    uint32_t hash = 2166136261u;
    if (!s) return hash;
    while (*s) {
        hash ^= (uint8_t)*s++;
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t hash_mix_u32(uint32_t hash, uint32_t value)
{
    hash ^= (value & 0xFFu);
    hash *= 16777619u;
    hash ^= ((value >> 8) & 0xFFu);
    hash *= 16777619u;
    hash ^= ((value >> 16) & 0xFFu);
    hash *= 16777619u;
    hash ^= ((value >> 24) & 0xFFu);
    hash *= 16777619u;
    return hash;
}

static void build_cache_path(const char *source_url, char *out, size_t out_size)
{
    snprintf(out, out_size, RSS_CACHE_DIR "/%08" PRIx32 ".bin", hash_url(source_url));
}

static bool read_cache_header(const char *source_url, rss_cache_header_t *out_header)
{
    char path[96] = {0};
    build_cache_path(source_url, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return false;

    rss_cache_header_t header = {0};
    size_t n = fread(&header, 1, sizeof(header), fp);
    fclose(fp);
    if (n != sizeof(header)) return false;
    if (header.magic != RSS_CACHE_MAGIC || header.version != RSS_CACHE_VERSION) return false;

    if (out_header) {
        *out_header = header;
    }
    return true;
}

static void cycle_state_free(void)
{
    for (int i = 0; i < RSS_CACHE_MAX_SOURCES; i++) {
        free(g_cycle_state.sources[i].shown_bits);
        g_cycle_state.sources[i].shown_bits = NULL;
        g_cycle_state.sources[i].item_count = 0;
        g_cycle_state.sources[i].shown_count = 0;
    }
    g_cycle_state.valid = false;
    g_cycle_state.signature = 0;
    g_cycle_state.source_count = 0;
    g_cycle_state.total_items = 0;
    g_cycle_state.remaining_items = 0;
}

static bool bit_get(const uint8_t *bits, uint32_t index)
{
    if (!bits) return false;
    return (bits[index / 8] & (1u << (index % 8))) != 0;
}

static void bit_set(uint8_t *bits, uint32_t index)
{
    if (!bits) return;
    bits[index / 8] |= (1u << (index % 8));
}

static uint32_t build_manifest_signature(const char *const *source_urls,
                                         int source_url_count,
                                         const rss_cache_header_t *headers,
                                         const bool *has_header)
{
    uint32_t sig = 2166136261u;
    sig = hash_mix_u32(sig, (uint32_t)source_url_count);

    for (int i = 0; i < source_url_count; i++) {
        uint32_t url_hash = hash_url(source_urls[i]);
        uint32_t item_count = 0;
        uint32_t updated_epoch = 0;
        if (has_header[i]) {
            item_count = headers[i].item_count;
            updated_epoch = headers[i].updated_epoch;
        }
        sig = hash_mix_u32(sig, url_hash);
        sig = hash_mix_u32(sig, item_count);
        sig = hash_mix_u32(sig, updated_epoch);
    }

    return sig;
}

static esp_err_t cycle_state_ensure(const char *const *source_urls, int source_url_count)
{
    if (!source_urls || source_url_count <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (source_url_count > RSS_CACHE_MAX_SOURCES) {
        source_url_count = RSS_CACHE_MAX_SOURCES;
    }

    rss_cache_header_t headers[RSS_CACHE_MAX_SOURCES] = {0};
    bool has_header[RSS_CACHE_MAX_SOURCES] = {0};
    for (int i = 0; i < source_url_count; i++) {
        if (source_urls[i] && source_urls[i][0] != '\0') {
            has_header[i] = read_cache_header(source_urls[i], &headers[i]);
        }
    }

    uint32_t signature = build_manifest_signature(
        source_urls, source_url_count, headers, has_header);

    if (g_cycle_state.valid &&
        g_cycle_state.signature == signature &&
        g_cycle_state.source_count == source_url_count) {
        return ESP_OK;
    }

    cycle_state_free();

    g_cycle_state.signature = signature;
    g_cycle_state.source_count = source_url_count;
    g_cycle_state.total_items = 0;
    g_cycle_state.remaining_items = 0;

    for (int i = 0; i < source_url_count; i++) {
        if (!has_header[i] || headers[i].item_count == 0) {
            continue;
        }

        g_cycle_state.sources[i].item_count = headers[i].item_count;
        g_cycle_state.sources[i].shown_count = 0;

        size_t bit_bytes = (size_t)((headers[i].item_count + 7u) / 8u);
        g_cycle_state.sources[i].shown_bits = calloc(1, bit_bytes);
        if (!g_cycle_state.sources[i].shown_bits) {
            cycle_state_free();
            return ESP_ERR_NO_MEM;
        }

        g_cycle_state.total_items += headers[i].item_count;
    }

    g_cycle_state.remaining_items = g_cycle_state.total_items;
    g_cycle_state.valid = true;
    return ESP_OK;
}

static void cycle_state_restart(void)
{
    if (!g_cycle_state.valid) return;

    for (int i = 0; i < g_cycle_state.source_count; i++) {
        cycle_source_state_t *src = &g_cycle_state.sources[i];
        if (!src->shown_bits || src->item_count == 0) continue;
        size_t bit_bytes = (size_t)((src->item_count + 7u) / 8u);
        memset(src->shown_bits, 0, bit_bytes);
        src->shown_count = 0;
    }

    g_cycle_state.remaining_items = g_cycle_state.total_items;
}

static bool contains_ci(const char *haystack, const char *needle)
{
    if (!haystack || !needle || needle[0] == '\0') return false;

    size_t nlen = strlen(needle);
    for (const char *h = haystack; *h; h++) {
        size_t i = 0;
        while (i < nlen &&
               h[i] &&
               tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == nlen) {
            return true;
        }
    }
    return false;
}

static uint8_t infer_item_flags(const rss_item_t *item)
{
    if (!item) return 0;

    // End-state markers take precedence over live markers.
    static const char *finished_markers[] = {
        " final",
        "final ",
        "final/",
        "postponed",
        "cancelled",
        "canceled",
        "suspended",
    };
    for (size_t i = 0; i < sizeof(finished_markers) / sizeof(finished_markers[0]); i++) {
        if (contains_ci(item->title, finished_markers[i]) ||
            contains_ci(item->description, finished_markers[i])) {
            return 0;
        }
    }

    static const char *live_markers[] = {
        "in progress",
        "halftime",
        "top ",
        "bottom ",
        "bot ",
        "end of ",
        "start of ",
        "q1",
        "q2",
        "q3",
        "q4",
        "1st period",
        "2nd period",
        "3rd period",
        "overtime",
        " ot ",
    };
    for (size_t i = 0; i < sizeof(live_markers) / sizeof(live_markers[0]); i++) {
        if (contains_ci(item->title, live_markers[i]) ||
            contains_ci(item->description, live_markers[i])) {
            return RSS_CACHE_ITEM_FLAG_LIVE;
        }
    }

    return 0;
}

static esp_err_t read_cache_record(const char *source_url, uint32_t item_index, rss_cache_record_t *out_rec)
{
    if (!source_url || !out_rec) return ESP_ERR_INVALID_ARG;

    char path[96] = {0};
    build_cache_path(source_url, path, sizeof(path));

    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_FAIL;

    rss_cache_header_t header = {0};
    if (fread(&header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return ESP_FAIL;
    }
    if (header.magic != RSS_CACHE_MAGIC || header.version != RSS_CACHE_VERSION) {
        fclose(fp);
        return ESP_FAIL;
    }
    if (item_index >= header.item_count) {
        fclose(fp);
        return ESP_ERR_INVALID_SIZE;
    }

    long offset = (long)sizeof(rss_cache_header_t) +
                  ((long)item_index * (long)sizeof(rss_cache_record_t));
    if (fseek(fp, offset, SEEK_SET) != 0) {
        fclose(fp);
        return ESP_FAIL;
    }

    size_t n = fread(out_rec, 1, sizeof(*out_rec), fp);
    fclose(fp);
    if (n != sizeof(*out_rec)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t rss_cache_init(void)
{
    int rc = mkdir(RSS_CACHE_DIR, 0775);
    if (rc != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create cache dir (%s): errno=%d", RSS_CACHE_DIR, errno);
        return ESP_FAIL;
    }
    cycle_state_free();
    return ESP_OK;
}

esp_err_t rss_cache_store_from_fetcher(const char *source_url, const char *source_name)
{
    if (!source_url || source_url[0] == '\0') return ESP_ERR_INVALID_ARG;

    int item_count = rss_get_count();
    if (item_count <= 0) {
        // Keep previous cache if feed is empty this cycle.
        return ESP_ERR_NOT_FOUND;
    }

    char final_path[96] = {0};
    char temp_path[112] = {0};
    build_cache_path(source_url, final_path, sizeof(final_path));
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", final_path);

    FILE *fp = fopen(temp_path, "wb");
    if (!fp) {
        ESP_LOGE(TAG, "Failed to open cache temp file: %s", temp_path);
        return ESP_FAIL;
    }

    rss_cache_header_t header = {
        .magic = RSS_CACHE_MAGIC,
        .version = RSS_CACHE_VERSION,
        .reserved = 0,
        .item_count = (uint32_t)item_count,
        .updated_epoch = (uint32_t)time(NULL),
    };

    if (fwrite(&header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        remove(temp_path);
        return ESP_FAIL;
    }

    for (int i = 0; i < item_count; i++) {
        const rss_item_t *src = rss_get_item(i);
        if (!src) continue;

        rss_cache_record_t rec;
        memset(&rec, 0, sizeof(rec));
        strncpy(rec.title, src->title, RSS_TITLE_LEN);
        rec.title[RSS_TITLE_LEN] = '\0';
        strncpy(rec.description, src->description, RSS_DESC_LEN);
        rec.description[RSS_DESC_LEN] = '\0';

        if (fwrite(&rec, 1, sizeof(rec), fp) != sizeof(rec)) {
            fclose(fp);
            remove(temp_path);
            return ESP_FAIL;
        }
    }

    fclose(fp);

    if (rename(temp_path, final_path) != 0) {
        // LittleFS may not replace existing files atomically.
        remove(final_path);
        if (rename(temp_path, final_path) != 0) {
            remove(temp_path);
            ESP_LOGE(TAG, "Failed to publish cache file: %s", final_path);
            return ESP_FAIL;
        }
    }

    // Cache content changed; rebuild no-repeat state on next pick.
    g_cycle_state.valid = false;

    ESP_LOGI(TAG, "Cached %d items for source '%s'", item_count, source_name ? source_name : source_url);
    return ESP_OK;
}

bool rss_cache_has_items_for_url(const char *source_url)
{
    rss_cache_header_t header = {0};
    if (!read_cache_header(source_url, &header)) return false;
    return header.item_count > 0;
}

esp_err_t rss_cache_pick_random_item_ex(const char *const *source_urls,
                                        int source_url_count,
                                        rss_item_t *out_item,
                                        int *out_source_index,
                                        uint8_t *out_flags,
                                        bool *out_cycle_reset)
{
    if (!source_urls || source_url_count <= 0 || !out_item) {
        return ESP_ERR_INVALID_ARG;
    }

    if (source_url_count > RSS_CACHE_MAX_SOURCES) {
        source_url_count = RSS_CACHE_MAX_SOURCES;
    }

    if (out_cycle_reset) *out_cycle_reset = false;
    if (out_flags) *out_flags = 0;

    esp_err_t state_err = cycle_state_ensure(source_urls, source_url_count);
    if (state_err != ESP_OK) {
        return state_err;
    }
    if (g_cycle_state.total_items == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    if (g_cycle_state.remaining_items == 0) {
        cycle_state_restart();
        if (out_cycle_reset) *out_cycle_reset = true;
    }

    uint32_t pick = esp_random() % g_cycle_state.remaining_items;
    int selected_source = -1;
    uint32_t selected_unshown_rank = 0;

    for (int i = 0; i < g_cycle_state.source_count; i++) {
        cycle_source_state_t *src = &g_cycle_state.sources[i];
        if (src->item_count == 0) continue;

        uint32_t remaining_in_source = src->item_count - src->shown_count;
        if (remaining_in_source == 0) continue;

        if (pick < remaining_in_source) {
            selected_source = i;
            selected_unshown_rank = pick;
            break;
        }
        pick -= remaining_in_source;
    }

    if (selected_source < 0) {
        return ESP_FAIL;
    }

    cycle_source_state_t *src = &g_cycle_state.sources[selected_source];
    uint32_t selected_item_index = UINT32_MAX;
    uint32_t rank = 0;
    for (uint32_t item_idx = 0; item_idx < src->item_count; item_idx++) {
        if (bit_get(src->shown_bits, item_idx)) continue;
        if (rank == selected_unshown_rank) {
            selected_item_index = item_idx;
            break;
        }
        rank++;
    }
    if (selected_item_index == UINT32_MAX) {
        return ESP_FAIL;
    }

    rss_cache_record_t rec = {0};
    esp_err_t read_err = read_cache_record(
        source_urls[selected_source], selected_item_index, &rec);
    if (read_err != ESP_OK) {
        return read_err;
    }

    bit_set(src->shown_bits, selected_item_index);
    src->shown_count++;
    g_cycle_state.remaining_items--;

    memset(out_item, 0, sizeof(*out_item));
    strncpy(out_item->title, rec.title, RSS_TITLE_LEN);
    out_item->title[RSS_TITLE_LEN] = '\0';
    strncpy(out_item->description, rec.description, RSS_DESC_LEN);
    out_item->description[RSS_DESC_LEN] = '\0';

    if (out_source_index) {
        *out_source_index = selected_source;
    }
    if (out_flags) {
        *out_flags = infer_item_flags(out_item);
    }
    return ESP_OK;
}

esp_err_t rss_cache_pick_random_item(const char *const *source_urls,
                                     int source_url_count,
                                     rss_item_t *out_item,
                                     int *out_source_index)
{
    return rss_cache_pick_random_item_ex(source_urls,
                                         source_url_count,
                                         out_item,
                                         out_source_index,
                                         NULL,
                                         NULL);
}

#include "rss_fetcher.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"

static const char *TAG = "rss_fetcher";

static rss_item_t rss_items[RSS_MAX_ITEMS];
static int rss_count = 0;

// ── HTML entity decoding ──

typedef struct {
    const char *entity;
    const char *replacement;
} entity_map_t;

static const entity_map_t entity_table[] = {
    {"&amp;",    "&"},
    {"&lt;",     "<"},
    {"&gt;",     ">"},
    {"&quot;",   "\""},
    {"&apos;",   "'"},
    {"&mdash;",  "-"},
    {"&ndash;",  "-"},
    {"&rsquo;",  "'"},
    {"&lsquo;",  "'"},
    {"&rdquo;",  "\""},
    {"&ldquo;",  "\""},
    {"&hellip;", "..."},
    {"&nbsp;",   " "},
    {"&copy;",   "(c)"},
    {"&reg;",    "(R)"},
    {"&deg;",    "deg"},
    {"&trade;",  "(TM)"},
    {NULL, NULL}
};

// Decode HTML entities in-place
static void decode_html_entities(char *str)
{
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '&') {
            // Try numeric entities: &#NNN; or &#xHHH;
            if (src[1] == '#') {
                char *semi = strchr(src, ';');
                if (semi && (semi - src) < 10) {
                    int codepoint = 0;
                    if (src[2] == 'x' || src[2] == 'X') {
                        codepoint = (int)strtol(src + 3, NULL, 16);
                    } else {
                        codepoint = (int)strtol(src + 2, NULL, 10);
                    }
                    if (codepoint >= 32 && codepoint <= 126) {
                        *dst++ = (char)codepoint;
                    } else {
                        *dst++ = '?';
                    }
                    src = semi + 1;
                    continue;
                }
            }

            // Try named entities
            bool found = false;
            for (int i = 0; entity_table[i].entity != NULL; i++) {
                size_t elen = strlen(entity_table[i].entity);
                if (strncmp(src, entity_table[i].entity, elen) == 0) {
                    const char *rep = entity_table[i].replacement;
                    size_t rlen = strlen(rep);
                    memcpy(dst, rep, rlen);
                    dst += rlen;
                    src += elen;
                    found = true;
                    break;
                }
            }
            if (!found) {
                *dst++ = *src++;
            }
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// ── UTF-8 to ASCII sanitization ──

static void sanitize_to_ascii(char *str)
{
    unsigned char *src = (unsigned char *)str;
    char *dst = str;
    bool last_was_space = false;

    // Skip leading whitespace
    while (*src && (*src == ' ' || *src == '\t' || *src == '\n' || *src == '\r')) {
        src++;
    }

    while (*src) {
        if (*src >= 32 && *src <= 126) {
            // Printable ASCII
            if (*src == ' ') {
                if (!last_was_space) {
                    *dst++ = ' ';
                    last_was_space = true;
                }
            } else {
                *dst++ = (char)*src;
                last_was_space = false;
            }
            src++;
        } else if (*src >= 0xC0) {
            // UTF-8 multi-byte sequence
            int seq_len = 0;
            unsigned char b0 = src[0];
            unsigned char b1 = (src[1]) ? src[1] : 0;
            unsigned char b2 = (src[1] && src[2]) ? src[2] : 0;

            if ((b0 & 0xE0) == 0xC0) seq_len = 2;       // 2-byte
            else if ((b0 & 0xF0) == 0xE0) seq_len = 3;   // 3-byte
            else if ((b0 & 0xF8) == 0xF0) seq_len = 4;   // 4-byte
            else { src++; continue; }  // invalid, skip

            // Check for common 3-byte UTF-8 sequences (U+2000 range)
            if (seq_len == 3 && b0 == 0xE2 && b1 == 0x80) {
                switch (b2) {
                case 0x93: *dst++ = '-'; break;         // en dash
                case 0x94: *dst++ = '-'; break;         // em dash
                case 0x98: *dst++ = '\''; break;        // left single quote
                case 0x99: *dst++ = '\''; break;        // right single quote
                case 0x9C: *dst++ = '"'; break;         // left double quote
                case 0x9D: *dst++ = '"'; break;         // right double quote
                case 0xA2: *dst++ = '*'; break;         // bullet
                case 0xA6:                              // ellipsis
                    *dst++ = '.'; *dst++ = '.'; *dst++ = '.';
                    break;
                default: break;  // skip unknown
                }
                last_was_space = false;
            }
            // Skip the full multi-byte sequence
            // Verify continuation bytes exist
            for (int i = 0; i < seq_len && *src; i++) src++;
        } else {
            // Non-ASCII single byte (0x80-0xBF orphan continuation, or control char)
            src++;
        }
    }

    // Trim trailing whitespace
    while (dst > str && (*(dst - 1) == ' ')) {
        dst--;
    }
    *dst = '\0';
}

// ── HTML tag stripping ──

static void strip_html_tags(char *str)
{
    char *src = str;
    char *dst = str;
    bool in_tag = false;

    while (*src) {
        if (*src == '<') {
            in_tag = true;
            src++;
        } else if (*src == '>' && in_tag) {
            in_tag = false;
            src++;
        } else if (!in_tag) {
            *dst++ = *src++;
        } else {
            src++;
        }
    }
    *dst = '\0';
}

// ── CDATA stripping ──

static void strip_cdata(char *str)
{
    // Remove <![CDATA[ prefix and ]]> suffix if present
    const char *cdata_start = "<![CDATA[";
    const char *cdata_end = "]]>";
    size_t start_len = strlen(cdata_start);

    char *p = str;
    char *dst = str;

    while (*p) {
        if (strncmp(p, cdata_start, start_len) == 0) {
            p += start_len;
        } else if (strncmp(p, cdata_end, 3) == 0) {
            p += 3;
        } else {
            *dst++ = *p++;
        }
    }
    *dst = '\0';
}

// ── XML extraction helpers ──

// Find content between <tag> and </tag>, starting from *pos.
// Returns pointer to content start, sets content_len. Advances *pos past </tag>.
// Returns NULL if tag not found.
static const char *extract_tag(const char *xml, const char **pos,
                                const char *tag_open, const char *tag_close,
                                int *content_len)
{
    const char *start = strstr(*pos, tag_open);
    if (!start) return NULL;
    start += strlen(tag_open);

    const char *end = strstr(start, tag_close);
    if (!end) return NULL;

    *content_len = (int)(end - start);
    *pos = end + strlen(tag_close);
    return start;
}

// Copy content to buffer, applying all text cleanup
static void extract_and_clean(const char *content, int content_len,
                               char *buf, int buf_size)
{
    int copy_len = (content_len < buf_size - 1) ? content_len : buf_size - 1;
    memcpy(buf, content, copy_len);
    buf[copy_len] = '\0';

    strip_cdata(buf);
    strip_html_tags(buf);
    decode_html_entities(buf);
    sanitize_to_ascii(buf);
}

// ── RSS XML parser ──

static void parse_rss_xml(const char *xml, int xml_len)
{
    rss_count = 0;
    const char *pos = xml;
    const char *xml_end = xml + xml_len;

    while (pos < xml_end && rss_count < RSS_MAX_ITEMS) {
        // Find next <item>
        const char *item_start = strstr(pos, "<item>");
        if (!item_start) break;

        const char *item_end = strstr(item_start, "</item>");
        if (!item_end) break;

        // Search within this item only
        const char *item_pos = item_start;
        int len;

        // Extract title
        const char *title = extract_tag(xml, &item_pos, "<title>", "</title>", &len);
        if (title && item_pos <= item_end) {
            extract_and_clean(title, len,
                              rss_items[rss_count].title, RSS_TITLE_LEN + 1);
        } else {
            rss_items[rss_count].title[0] = '\0';
        }

        // Extract description
        item_pos = item_start;  // reset to search from item start
        const char *desc = extract_tag(xml, &item_pos, "<description>", "</description>", &len);
        if (desc && item_pos <= item_end + 13) {  // +13 for </description> tag length
            extract_and_clean(desc, len,
                              rss_items[rss_count].description, RSS_DESC_LEN + 1);
        } else {
            rss_items[rss_count].description[0] = '\0';
        }

        // Only count items that have at least a title
        if (rss_items[rss_count].title[0] != '\0') {
            rss_count++;
        }

        pos = item_end + 7;  // past </item>
    }

    ESP_LOGI(TAG, "Parsed %d RSS items", rss_count);
}

// ── HTTP client event handler ──

typedef struct {
    char *buffer;
    int buffer_len;
    int buffer_size;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp->buffer && (resp->buffer_len + evt->data_len < resp->buffer_size)) {
            memcpy(resp->buffer + resp->buffer_len, evt->data, evt->data_len);
            resp->buffer_len += evt->data_len;
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ── Public API ──

esp_err_t rss_fetch(const char *url)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "No RSS URL configured");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Fetching RSS: %s", url);

    // Allocate receive buffer (64KB max)
    const int buf_size = 64 * 1024;
    http_response_t resp = {
        .buffer = malloc(buf_size),
        .buffer_len = 0,
        .buffer_size = buf_size,
    };

    if (!resp.buffer) {
        ESP_LOGE(TAG, "Failed to allocate HTTP buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp.buffer);
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP status: %d, content length: %d", status, resp.buffer_len);

        if (status == 200 && resp.buffer_len > 0) {
            resp.buffer[resp.buffer_len] = '\0';  // null-terminate
            parse_rss_xml(resp.buffer, resp.buffer_len);
        } else {
            ESP_LOGE(TAG, "HTTP error: status=%d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    free(resp.buffer);
    return err;
}

int rss_get_count(void)
{
    return rss_count;
}

const rss_item_t *rss_get_item(int index)
{
    if (index < 0 || index >= rss_count) {
        return NULL;
    }
    return &rss_items[index];
}

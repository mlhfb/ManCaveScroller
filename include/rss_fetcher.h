#ifndef RSS_FETCHER_H
#define RSS_FETCHER_H

#include "esp_err.h"

#define RSS_MAX_ITEMS    30
#define RSS_TITLE_LEN    200
#define RSS_DESC_LEN     200

typedef struct {
    char title[RSS_TITLE_LEN + 1];
    char description[RSS_DESC_LEN + 1];
} rss_item_t;

// Fetch and parse an RSS feed. Call with WiFi connected.
esp_err_t rss_fetch(const char *url);

// Number of items parsed from last successful fetch (0 if none)
int rss_get_count(void);

// Get parsed item by index. Returns NULL if out of range.
const rss_item_t *rss_get_item(int index);

#endif

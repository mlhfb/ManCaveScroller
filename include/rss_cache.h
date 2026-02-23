#ifndef RSS_CACHE_H
#define RSS_CACHE_H

#include <stdbool.h>
#include "esp_err.h"
#include "rss_fetcher.h"

#define RSS_CACHE_ITEM_FLAG_LIVE 0x01

esp_err_t rss_cache_init(void);

// Store currently parsed rss_fetcher items under a cache key derived from source_url.
esp_err_t rss_cache_store_from_fetcher(const char *source_url, const char *source_name);

// Check whether cached data exists for this URL and has at least one item.
bool rss_cache_has_items_for_url(const char *source_url);

// Pick one random item across all provided source URLs (weighted by item count)
// without repeats until all cached items have been shown once.
esp_err_t rss_cache_pick_random_item(const char *const *source_urls,
                                     int source_url_count,
                                     rss_item_t *out_item,
                                     int *out_source_index);

// Extended pick API with flags for future scheduling rules.
esp_err_t rss_cache_pick_random_item_ex(const char *const *source_urls,
                                        int source_url_count,
                                        rss_item_t *out_item,
                                        int *out_source_index,
                                        uint8_t *out_flags,
                                        bool *out_cycle_reset);

#endif

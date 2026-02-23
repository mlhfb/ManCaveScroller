# TODO

## RSS/Cache
- [ ] Add cache metadata to `/api/status` (per feed last refresh timestamp, item count, and stale/healthy state).
- [ ] Show cache status in `littlefs/web/index.html` so users can confirm feeds are updating.
- [ ] Add cache TTL/expiry policy so stale feeds are detected after a configurable age.
- [ ] Keep serving cached items during outages, but flag stale feeds in status/UI.
- [ ] Add hot-list team priority mode (show live games every X items using `RSS_CACHE_ITEM_FLAG_LIVE`).

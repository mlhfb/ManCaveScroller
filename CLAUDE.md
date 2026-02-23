# ManCaveScroller

ESP32 LED ticker with local web configuration, LittleFS-hosted assets, and cache-backed RSS/sports feeds.

## Hardware
- Board: ESP32 DoIt DevKit V1
- Display: WS2812B matrix panels (8 rows x 32 columns each), serpentine layout
- Supported widths: 32, 64, 96, 128 columns
- LED data pin: GPIO 5 (set by `-DLED_STRIP_GPIO=5` in `platformio.ini`)
- Config button: BOOT button (GPIO 0)

## Firmware Stack
- Framework: ESP-IDF 5.1.1 (no Arduino)
- Language: C + FreeRTOS primitives
- Build system: PlatformIO (`framework = espidf`)
- Web server: `esp_http_server`
- HTTP client: `esp_http_client` with CRT bundle
- Local assets/filesystem: LittleFS (`esp_littlefs`)
- Settings storage: NVS (`namespace = mancave`)

## Flash Layout
From `partitions.csv`:
- `factory` app partition: 0x200000 (2 MB)
- `littlefs` data partition: 0x1F0000 (~1.94 MB)
- Total device flash: 4 MB

This is why app-size reporting uses a 2 MB app limit, not 4 MB.

## External Sports Backend
This project integrates with:
- `https://github.com/mlhfb/ManCaveBackEnd`

Primary endpoint expected by this firmware:
- `.../espn_scores_rss.php?sport=<sport>&format=rss`

Supported sports used here:
- `mlb`, `nhl`, `ncaaf`, `nfl`, `nba`, `big10`

URL input behavior in firmware settings:
- If user enters host/path only, firmware prepends `https://` (if missing) and appends `espn_scores_rss.php`.
- If user enters an explicit `.php` path, firmware uses it directly and only appends query params.

## Runtime Architecture
- Main control loop lives in `src/main.c` (`app_main`), not a dedicated display task.
- `scroller_tick()` drives frame updates (~16 ms cadence).
- WiFi runs as ESP-IDF background tasks.
- In STA mode, WiFi radio is usually off during scrolling to reduce display glitches.

Content pipeline:
1. Settings build enabled RSS source list in deterministic order.
2. On refresh cycle, each source is fetched and cached to LittleFS (`/littlefs/cache`).
3. Display pulls random cached items (title then description).
4. Random picker is no-repeat until all cached items are shown once, then resets.
5. On outages, cached data is still used.
6. If no cache is available, falls back to custom messages.

## RSS/Cache Notes
- Cache module: `src/rss_cache.c` / `include/rss_cache.h`
- Cache keying: per-source URL hash
- Selection API:
  - `rss_cache_pick_random_item(...)`
  - `rss_cache_pick_random_item_ex(...)` (returns flags + cycle-reset info)
- Current item flag reserved for future scheduling:
  - `RSS_CACHE_ITEM_FLAG_LIVE` (heuristic from title/description text)

## Settings Model (high-level)
`app_settings_t` includes:
- 5 custom messages (text/color/enabled)
- speed, brightness, panel width
- WiFi credentials
- RSS global toggle
- NPR URL + `rss_npr_enabled`
- Sports toggle + base URL + per-sport toggles (`mlb`, `nhl`, `ncaaf`, `nfl`, `nba`, `big10`)
- materialized `rss_sources[]` list for runtime

## Web UI + API
UI source:
- `littlefs/web/index.html`

Served from:
- `GET /` -> `LITTLEFS_WEB_INDEX_PATH`

Primary API endpoints:
- `GET /api/status`
- `POST /api/messages`
- `POST /api/text` (legacy)
- `POST /api/color` (legacy)
- `POST /api/speed`
- `POST /api/brightness`
- `POST /api/appearance`
- `POST /api/wifi`
- `POST /api/advanced`
- `POST /api/rss`
- `POST /api/factory-reset`

`/api/rss` payload supports:
- `enabled`
- `url` (NPR URL)
- `npr_enabled`
- `sports_enabled`
- `sports_base_url`
- `sports` object with booleans: `mlb`, `nhl`, `ncaaf`, `nfl`, `nba`, `big10`

## Project Layout (current)
- `src/main.c` - boot orchestration, display loop, RSS refresh/playback orchestration
- `src/led_panel.c` - WS2812B RMT driver + framebuffer mapping
- `src/text_scroller.c` - scrolling renderer/timing
- `src/settings.c` - defaults, NVS load/save, RSS source manifest building
- `src/wifi_manager.c` - AP/STA lifecycle, captive DNS, radio on/off
- `src/web_server.c` - HTTP endpoints + JSON handlers
- `src/rss_fetcher.c` - RSS HTTP fetch + XML parse/sanitize
- `src/rss_cache.c` - LittleFS cache + non-repeating random picker
- `include/storage_paths.h` - LittleFS path constants
- `littlefs/` - deployed web/font/config assets

## Build/Flash Commands
- Build: `pio run`
- Upload firmware: `pio run -t upload`
- Upload filesystem image: `pio run -t uploadfs`
- Serial monitor: `pio device monitor -b 115200`

## Important Practical Notes
- Keep WiFi off during normal scrolling in STA mode to reduce LED artifacts.
- If RSS behavior changes, update all three layers together:
  - `littlefs/web/index.html`
  - `src/web_server.c`
  - `src/settings.c`
- If sports backend contract changes, sync parser assumptions and UI labels.

# ManCaveScroller

WiFi-controlled scrolling LED display for a man cave.

## Hardware
- **Board:** ESP32 DoIt DevKit V1
- **Display:** WS2812B 8x32 LED panel (256 pixels, serpentine/zigzag layout)
- **Data pin:** GPIO 13 (configurable via `-DLED_STRIP_GPIO=XX` in platformio.ini)

## Tech Stack
- **Framework:** ESP-IDF 5.1.1 (NOT Arduino)
- **Language:** C with FreeRTOS
- **Build:** PlatformIO (`pio run` to build, Upload button in VS Code to flash)
- **PlatformIO CLI path:** `C:\Users\mikelch\.platformio\penv\Scripts\pio.exe`

## Project Structure
```
src/
  main.c              - Boot orchestration + main display loop (drives scroller directly)
  led_panel.c         - Custom RMT driver for WS2812B, framebuffer[8][32], serpentine mapping
  font.c              - 5x7 bitmap font, 95 ASCII glyphs (space through tilde), column-major
  text_scroller.c     - Main-loop-driven horizontal scrolling, mutex-protected settings
  settings.c          - NVS persistence (namespace "mancave")
  wifi_manager.c      - AP/STA dual mode, captive portal DNS, radio on/off cycling
  rss_fetcher.c       - HTTPS RSS feed fetcher, XML parser, HTML entity decoder
  web_server.c        - esp_http_server with JSON API endpoints, uses cJSON
  CMakeLists.txt      - ESP-IDF component registration + dependencies
include/
  led_panel.h         - Framebuffer API (init, set_pixel, refresh, set_brightness, set_cols)
  font.h              - font_get_glyph() returns 5-byte column data per character
  text_scroller.h     - Scroller API (set_text, set_color, set_speed, tick)
  settings.h          - app_settings_t struct, load/save to NVS
  wifi_manager.h      - WiFi mode control (AP/STA/connecting), radio_on/radio_off
  rss_fetcher.h       - RSS fetch API and item struct
  web_server.h        - Server start/stop
  web_page.h          - Embedded HTML/CSS/JS dark theme UI (single-page, inline everything)
```

## Architecture
- Display driven from `app_main()` while(1) loop — NOT a FreeRTOS task
- `scroller_tick()` renders one frame and returns delay_ms; main loop calls `vTaskDelay()`
- WiFi/web server run as ESP-IDF background tasks
- WiFi must be OFF during scrolling (RMT ISR conflicts with WiFi ISRs)
- RSS fetch: radio_on → HTTP GET → radio_off, then scroll all items with WiFi off
- When intentionally stopping WiFi, set `radio_cycling=true` + `sta_retry_count=1` before `esp_wifi_stop()` to suppress disconnect handler retries

## Key Design Decisions
- **No external LED library** - custom RMT bytes encoder for WS2812B timing (10MHz, bit0=3/9, bit1=9/3)
- **Serpentine mapping** - even rows L->R, odd rows R->L (flip in led_panel.c if panel differs)
- **Web UI embedded in C** - `web_page.h` contains full HTML as a const string, no SPIFFS needed
- **mdns not available** - it's a managed component in ESP-IDF 5.1.1, not bundled
- **Default brightness: 32/255** - conservative to avoid high current draw (256 LEDs at full = ~15A)
- **RMT mem_block_symbols MUST be 256** (not 64) for WiFi coexistence

## API Endpoints
| Method | URI               | Purpose                              |
|--------|-------------------|--------------------------------------|
| GET    | /                 | Serve web UI                         |
| GET    | /api/status       | JSON: messages, speed, WiFi, RSS     |
| POST   | /api/messages     | Update all 5 messages                |
| POST   | /api/text         | Update message[0] text (legacy)      |
| POST   | /api/color        | Update message[0] color (legacy)     |
| POST   | /api/speed        | Update scroll speed (1-10)           |
| POST   | /api/brightness   | Update brightness (1-255)            |
| POST   | /api/appearance   | Set speed + brightness together      |
| POST   | /api/wifi         | Set WiFi credentials                 |
| POST   | /api/advanced     | Set panel size (32/64/96/128)        |
| POST   | /api/rss          | Enable/configure RSS feed            |
| POST   | /api/factory-reset| Erase NVS and restart                |

## WiFi Behavior
1. On boot: check NVS for stored WiFi credentials
2. If found: try STA mode (15s timeout, 5 retries), then stop WiFi radio
3. If none or failed: start AP mode as "ManCave" (open, no password)
4. AP mode includes captive portal DNS (redirects all domains to 192.168.4.1)
5. STA mode: WiFi off during scrolling, BOOT button toggles config mode
6. Config mode: WiFi on, web server accessible, press BOOT again to exit

## Build Notes
- Full rebuild takes ~5-10 minutes (ESP-IDF compiles everything)
- Incremental builds are much faster (seconds for source-only changes)
- Flash usage ~97% with WiFi+HTTP+TLS+RSS included
- RAM usage ~16%

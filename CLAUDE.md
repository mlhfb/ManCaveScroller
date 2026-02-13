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
  main.c              - Boot orchestration: NVS -> settings -> LED -> scroller -> WiFi -> web server
  led_panel.c         - Custom RMT driver for WS2812B, framebuffer[8][32], serpentine mapping
  font.c              - 5x7 bitmap font, 95 ASCII glyphs (space through tilde), column-major
  text_scroller.c     - FreeRTOS task for horizontal scrolling, mutex-protected settings
  settings.c          - NVS persistence (namespace "mancave")
  wifi_manager.c      - AP/STA dual mode, captive portal DNS server on port 53
  web_server.c        - esp_http_server with JSON API endpoints, uses cJSON
  CMakeLists.txt      - ESP-IDF component registration + dependencies
include/
  led_panel.h         - Framebuffer API (init, set_pixel, refresh, set_brightness)
  font.h              - font_get_glyph() returns 5-byte column data per character
  text_scroller.h     - Scroller API (set_text, set_color, set_speed, start/stop)
  settings.h          - app_settings_t struct, load/save to NVS
  wifi_manager.h      - WiFi mode control (AP/STA/connecting)
  web_server.h        - Server start/stop
  web_page.h          - Embedded HTML/CSS/JS dark theme UI (single-page, inline everything)
```

## Key Design Decisions
- **No external LED library** - custom RMT bytes encoder for WS2812B timing (10MHz, bit0=3/9, bit1=9/3)
- **Serpentine mapping** - even rows L->R, odd rows R->L (flip in led_panel.c if panel differs)
- **Web UI embedded in C** - `web_page.h` contains full HTML as a const string, no SPIFFS needed
- **mdns not available** - it's a managed component in ESP-IDF 5.1.1, not bundled
- **Default brightness: 32/255** - conservative to avoid high current draw (256 LEDs at full = ~15A)

## API Endpoints
| Method | URI              | Purpose                    |
|--------|------------------|----------------------------|
| GET    | /                | Serve web UI               |
| GET    | /api/status      | JSON: text, color, speed, WiFi status |
| POST   | /api/text        | Update scroll text         |
| POST   | /api/color       | Update RGB color           |
| POST   | /api/speed       | Update scroll speed (1-10) |
| POST   | /api/brightness  | Update brightness (1-255)  |
| POST   | /api/wifi        | Set WiFi credentials       |

## WiFi Behavior
1. On boot: check NVS for stored WiFi credentials
2. If found: try STA mode (15s timeout, 5 retries)
3. If none or failed: start AP mode as "ManCaveScroller" (open, no password)
4. AP mode includes captive portal DNS (redirects all domains to 192.168.4.1)

## Build Notes
- Full rebuild takes ~5-10 minutes (ESP-IDF compiles everything)
- Incremental builds are much faster (seconds for source-only changes)
- Flash usage ~81% with WiFi+HTTP+TLS included
- RAM usage ~10%

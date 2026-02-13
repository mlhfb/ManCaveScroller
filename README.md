# ManCaveScroller

WiFi-controlled scrolling LED display built on ESP32. Set your message, color, speed, and brightness from any device on your network via a dark-themed web UI.

![ESP32](https://img.shields.io/badge/ESP32-PlatformIO-orange) ![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.1.1-blue) ![License](https://img.shields.io/badge/license-MIT-green)

## Features

- **Scrolling text** on a WS2812B 8x32 LED panel (256 pixels)
- **Multiple messages** — up to 5 custom messages, each with its own color, cycling automatically
- **Web UI** for real-time control — messages, color, speed, brightness
- **AP + STA WiFi** — creates its own hotspot for setup, then connects to your network
- **Captive portal** — auto-redirects to the config page when connected to the AP
- **Persistent settings** — saved to NVS flash, survives reboots
- **Config mode via BOOT button** — press to enable WiFi and access the web UI, press again to resume glitch-free scrolling
- **No external dependencies** — custom RMT driver, embedded web page, no SPIFFS

## Hardware

| Component | Details |
|-----------|---------|
| **Board** | ESP32 DoIt DevKit V1 |
| **Display** | WS2812B 8x32 LED panel (serpentine/zigzag layout) |
| **Data pin** | GPIO 5 (configurable in `platformio.ini`) |
| **Power** | 5V, adequate supply for 256 LEDs (default brightness is conservative) |

## Quick Start

### Prerequisites
- [VS Code](https://code.visualstudio.com/) with [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- ESP32 DevKit connected via USB

### Build & Flash
```bash
# Build
pio run

# Upload to ESP32
pio run -t upload

# Monitor serial output
pio device monitor -b 115200
```

### First Boot
1. The ESP32 starts in **AP mode** — look for the `ManCave` WiFi network (open, no password)
2. Connect to it and open `http://192.168.4.1` (or wait for the captive portal redirect)
3. Set your scroll text, color, speed, and brightness
4. Optionally enter your home WiFi credentials to switch to **STA mode**
5. Settings persist across reboots

## Web API

| Method | Endpoint | Body | Purpose |
|--------|----------|------|---------|
| `GET` | `/` | — | Web UI |
| `GET` | `/api/status` | — | Current settings, messages, WiFi status |
| `POST` | `/api/messages` | `{"messages":[...]}` | Update all 5 messages (text, color, enabled) |
| `POST` | `/api/text` | `{"text":"Hello!"}` | Set message 1 text (legacy) |
| `POST` | `/api/color` | `{"r":255,"g":0,"b":0}` | Set message 1 color (legacy) |
| `POST` | `/api/speed` | `{"speed":5}` | Set scroll speed (1-10) |
| `POST` | `/api/brightness` | `{"brightness":32}` | Set brightness (1-255) |
| `POST` | `/api/wifi` | `{"ssid":"...","password":"..."}` | Connect to WiFi |

## Project Structure

```
src/
  main.c            Main loop drives the display directly (no FreeRTOS task)
  led_panel.c       Custom RMT driver for WS2812B, framebuffer, serpentine mapping
  font.c            5x7 bitmap font, 95 ASCII glyphs, column-major encoding
  text_scroller.c   Horizontal scrolling engine, mutex-protected settings
  settings.c        NVS persistence (namespace "mancave")
  wifi_manager.c    AP/STA dual mode, captive portal DNS
  web_server.c      esp_http_server with JSON API endpoints (cJSON)
include/
  web_page.h        Embedded HTML/CSS/JS dark theme UI (single const string)
  led_panel.h       Framebuffer API
  font.h            Glyph lookup
  text_scroller.h   Scroller control API
  settings.h        Settings struct and NVS load/save
  wifi_manager.h    WiFi mode control
  web_server.h      Server start/stop
```

## Architecture

The display is driven directly from the `app_main()` loop — inspired by how vintage 1970s/80s home computers used the CPU to drive the display and ran other code during blanking periods.

- **Main loop** calls `scroller_tick()` each frame, which renders to the framebuffer and returns the delay until the next frame
- **WiFi and web server** run as ESP-IDF background tasks
- **RMT peripheral** generates precise WS2812B timing via a bytes encoder (10MHz, no external library)
- **Shared state** (text, color, speed) is protected by a FreeRTOS mutex

## Configuration

Edit `platformio.ini` to change the LED data pin:
```ini
build_flags =
    -DLED_STRIP_GPIO=5
```

Default brightness is 32/255 — conservative to keep current draw manageable. 256 LEDs at full white and full brightness can draw up to 15A.

## WiFi Behavior

1. On boot, checks NVS for stored WiFi credentials
2. If found, attempts STA connection (15s timeout, 5 retries)
3. If none stored or connection fails, starts AP mode as **"ManCave"** (open)
4. AP mode includes a captive portal DNS server that redirects all domains to `192.168.4.1`
5. In STA mode, WiFi is turned **off** after connecting to eliminate display glitches
6. Press the **BOOT button** to enter config mode — WiFi reconnects, web UI becomes accessible
7. Press **BOOT again** to exit config mode — WiFi off, settings applied, scrolling resumes

## License

MIT

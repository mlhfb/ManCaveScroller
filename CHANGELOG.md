# Changelog

## [Unreleased]

### Added
- **Multi-message support** — configure up to 5 scrolling messages, each with its own text, color, and enable/disable toggle (fixes #3)
  - Messages cycle automatically: when one finishes scrolling, the next enabled message starts
  - New "Edit Messages" button on the web UI opens a dedicated editor view
  - Each message row has a checkbox (enable), text input, and color picker
  - New `/api/messages` endpoint for reading/updating all messages
  - NVS storage migrates seamlessly from old single-message format
- **Password visibility toggle** — eye icon button on the WiFi password field to show/hide the password (fixes #2)
- **BOOT button config mode** — press the BOOT button (GPIO 0) on the ESP32 DevKit to toggle configuration mode
  - Enters config mode: WiFi reconnects, display shows IP address for web UI access
  - Exits config mode: WiFi turns off, any settings changed via web UI are applied
  - 300ms debounce via GPIO interrupt on falling edge

### Changed
- WiFi is now fully off during normal scrolling in STA mode (eliminates display glitches caused by WiFi ISRs interfering with RMT timing)
- `wifi_manager_radio_on()` now returns `bool` indicating connection success/failure
- `wifi_manager_radio_on()` no longer auto-closes after 2 seconds — stays connected until `radio_off()` is called
- Settings struct changed from single text+color to `messages[5]` array with per-message color and enable flag
- Web UI main view shows a preview of enabled messages with their colors
- `/api/status` now returns a `messages` array instead of single `text` and `color` fields
- Legacy `/api/text` and `/api/color` endpoints still work (target message slot 0)
- POST body max size increased from 512 to 4096 bytes to accommodate multi-message payloads

### Fixed
- Display glitches (random colored pixels) caused by WiFi radio interrupts preempting the RMT encoder — resolved by keeping WiFi off during display operation (fixes #1)

## [1.0.0] - 2026-02-12

### Added
- Initial release
- Scrolling text on WS2812B 8x32 LED panel (256 pixels)
- Web UI for real-time control (text, color, speed, brightness)
- AP + STA dual-mode WiFi with captive portal
- Persistent settings via NVS flash
- Custom RMT driver for WS2812B (no external dependencies)
- 5x7 bitmap font covering printable ASCII

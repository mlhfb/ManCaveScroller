# Changelog

## [Unreleased]

### Added
- **RSS news feed support** — scroll headlines from any RSS feed (e.g., NPR, BBC)
  - Enable and configure via the Advanced page in the web UI
  - Each RSS item scrolls its title then description, with rotating colors (white, yellow, green, red, blue, cyan, violet)
  - Custom messages interleave after every 4th news item, cycling sequentially
  - WiFi turns on briefly to fetch the feed, then off for glitch-free scrolling
  - After all items display, the feed re-fetches automatically
  - Full HTML entity decoding and UTF-8 to ASCII sanitization for clean display
  - New `POST /api/rss` endpoint for enabling/configuring RSS
  - RSS status included in `/api/status` response
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
- **Advanced settings page** — accessible from the main web UI
  - **Panel size selector** — configure for 1–4 chained panels (8x32, 8x64, 8x96, 8x128)
  - **Factory Default** button — erases NVS and restarts the device (with confirmation dialog)
  - New `/api/appearance` endpoint for combined speed + brightness save
  - New `/api/advanced` endpoint for panel size configuration
  - New `/api/factory-reset` endpoint for full device reset

### Changed
- WiFi is now fully off during normal scrolling in STA mode (eliminates display glitches caused by WiFi ISRs interfering with RMT timing)
- `wifi_manager_radio_on()` now returns `bool` indicating connection success/failure
- `wifi_manager_radio_on()` no longer auto-closes after 2 seconds — stays connected until `radio_off()` is called
- Settings struct changed from single text+color to `messages[5]` array with per-message color and enable flag
- Web UI main view shows a preview of enabled messages with their colors
- `/api/status` now returns a `messages` array instead of single `text` and `color` fields, plus `wifi_password` and `panel_cols`
- Legacy `/api/text` and `/api/color` endpoints still work (target message slot 0)
- POST body max size increased from 512 to 4096 bytes to accommodate multi-message payloads
- Appearance section now uses a single "Save" button for both speed and brightness
- LED panel driver supports runtime column count (32–128) for multi-panel configurations
- WiFi password auto-populates on page load from saved settings

### Fixed
- Display glitches (random colored pixels) caused by WiFi radio interrupts preempting the RMT encoder — resolved by keeping WiFi off during display operation (fixes #1)
- WiFi disconnect handler race condition — `esp_wifi_stop()` during intentional radio cycling triggered spurious retry attempts with 2-second blocking delays; now suppressed via `radio_cycling` flag
- Default settings now enable RSS with NPR feed URL pre-configured (previously blank, causing confusion with placeholder text)
- RSS URL validation in web UI — warns if RSS is enabled without a URL entered
- NVS error checking added for RSS URL save operations
- Disabled ANSI color escape codes in serial log output (`CONFIG_LOG_COLORS`) to prevent garbled terminal output

## [1.0.0] - 2026-02-12

### Added
- Initial release
- Scrolling text on WS2812B 8x32 LED panel (256 pixels)
- Web UI for real-time control (text, color, speed, brightness)
- AP + STA dual-mode WiFi with captive portal
- Persistent settings via NVS flash
- Custom RMT driver for WS2812B (no external dependencies)
- 5x7 bitmap font covering printable ASCII

# Changelog

## [Unreleased]

### Added
- **BOOT button config mode** — press the BOOT button (GPIO 0) on the ESP32 DevKit to toggle configuration mode
  - Enters config mode: WiFi reconnects, display shows IP address for web UI access
  - Exits config mode: WiFi turns off, any settings changed via web UI are applied
  - 300ms debounce via GPIO interrupt on falling edge
- Display shows "Press button to enter configuration" in normal scrolling mode

### Changed
- WiFi is now fully off during normal scrolling in STA mode (eliminates display glitches caused by WiFi ISRs interfering with RMT timing)
- `wifi_manager_radio_on()` now returns `bool` indicating connection success/failure
- `wifi_manager_radio_on()` no longer auto-closes after 2 seconds — stays connected until `radio_off()` is called

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

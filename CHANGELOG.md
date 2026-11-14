# Changelog

All notable changes to KAST are documented here. The format is loosely based
on [Keep a Changelog](https://keepachangelog.com/), and the project follows
[Semantic Versioning](https://semver.org/).

## [0.1.0] — 2026-11-13

First public release — the complete desk terminal: live Kick chat with
moderation, OBS scene control, soundboard, motion overlays, and the KAST
Companion for Windows.

### Added

**Firmware (ESP32-4848S040, ESP-IDF v6.1):**
- ST7701S RGB display driver: vendor init over a 3-wire SPI command channel,
  480×480 RGB565 in PSRAM framebuffers, 12 MHz PCLK, LEDC PWM backlight
  with a staged boot ramp
- GT911 capacitive touch (I²C 0x5D on GPIO19/45, no INT/RST lines)
- LVGL 9 UI: status bar on every screen (Wi-Fi / relay / OBS link state,
  PANIC kill switch), screen router, Home showcase, settings page with
  on-device Wi-Fi and relay entry -> NVS
- Kick chat terminal: scrolling view, emote rendering, QWERTY composer,
  quick-reply macros edited on-device, long-press moderation (delete /
  ban / timeout)
- Scene controller: full-screen OBS scene tiles in studio-UI order with
  live-scene highlight
- SFX soundboard and motion-graphics triggers, with CLEAR and PANIC
  moderation controls
- SDSPI + FATFS micro SD support; WebSocket client with auto-reconnect,
  backoff, and ping; `idf_run.py` Git Bash wrapper for `idf.py`

**PC relay (Node.js):**
- Device WebSocket link (port 8765) with chat frames, commands, and health
- Kick API wiring: OAuth 2.1 + PKCE sign-in, live chat read, send, and
  moderation injection (credentials stay in gitignored `.env`)
- OBS WebSocket v5 client: scene listing/switching, managed `KAST SFX` /
  `KAST GFX` scenes and sources, media playback, and PANIC (full-screen
  cover + mic mute + stop-all-media)
- 12-slot sound and graphics libraries with guided add/replace/remove and
  test coverage
- 12 starter sound effects

**KAST Companion (Windows tray app):**
- Zero-terminal tray host: auto-discovers the box, starts the relay,
  handles the firewall, and shows link status
- Guided first-run wizard: detect -> flash -> verify -> OBS setup ->
  Kick sign-in
- Sound Library, Graphics Library, and Scene Tile picker windows
- `Start KAST` one-click launcher (OBS + relay)

**Installer:**
- Self-contained `KAST-Setup.exe` (Inno Setup): portable Node.js, firmware
  images with one-click flasher, CH340 USB driver

**Repository:**
- GitHub Actions CI: every push to `main` builds in the official
  `espressif/idf:v6.1` container; every `v*` tag auto-cuts a release with
  flashable binaries

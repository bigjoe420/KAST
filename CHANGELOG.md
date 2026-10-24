# Changelog

All notable changes to KAST are documented here. The format is loosely based
on [Keep a Changelog](https://keepachangelog.com/), and the project follows
[Semantic Versioning](https://semver.org/).

## [0.1.0] — 2026-10-24

Initial bring-up for the JCZN ESP32-4848S040 (ESP32-S3-N16R8).

### Added

- ESP-IDF v6.1 project skeleton with mandatory sdkconfig contract
  (16 MB flash, octal PSRAM)
- ST7701S RGB display driver: vendor type1 init sequence over a 3-wire SPI
  command channel (SPI mode 0), 480×480 RGB565 in a PSRAM framebuffer,
  12 MHz PCLK from PLL160M, LEDC PWM backlight
- GT911 capacitive touch: I²C at 0x5D on GPIO19/45, raw register polling,
  tap history + statistics exposed over the serial log for bench verification
- `idf_run.py` — location-independent Git Bash wrapper for `idf.py`
  (ESP-IDF v6.1 refuses MSYSTEM)
- GitHub Actions CI: every push to `main` builds in the official
  `espressif/idf:v6.1` container; every `v*` tag auto-cuts a release with
  flashable binaries

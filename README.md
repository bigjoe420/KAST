# KAST — Kick Auxiliary Streaming Terminal (ESP32-S3)

> Streamer's desk companion: live Kick chat with moderation tools, OBS scene
> control, and an SFX soundboard on a 4-inch round touchscreen — no browser
> tabs, no second monitor.

**Board:** JCZN **ESP32-4848S040** — ESP32-S3-N16R8 (16 MB flash, 8 MB PSRAM)
with a 4.0" 480×480 round IPS LCD (ST7701S RGB interface), GT911 capacitive
touch, I²S speaker amplifier (NS4168), micro SD slot, and USB-C.

## Features

| | |
|---|---|
| **Kick Chat Terminal** | Live chat over WebSocket via the PC relay, scrolling message view, moderator actions (delete / ban / timeout), quick-reply macro row, and a pop-up QWERTY keyboard scaled for the 480×480 display. |
| **Scene Controller** | One-tap OBS scene switching and source muting over the local OBS WebSocket API — layouts like "Just Chatting", "Main Game", and "BRB" with active-state highlighting. |
| **SFX Soundboard** | Six labeled trigger slots: desk-side audio over I²S to the on-board amp, stream-side audio via OBS media sources, so the audience hears exactly what you intend. |
| **PC relay layer** | A lightweight local server on the streaming PC owns Kick API authentication and raw JSON parsing, feeding the terminal clean text and triggers — the microcontroller's workload stays light. |
| **Connection status** | Persistent status bar: Wi-Fi, relay, and OBS link state at a glance. |
| **Pure C, no IDE** | ESP-IDF v6.1, FreeRTOS task-per-subsystem, LVGL on a PSRAM framebuffer. |

## Current Status

Hardware bring-up is **complete and verified on the bench**: the ST7701S
display and GT911 touch controller both run simultaneously with clean
coordinates across the full panel. UI framework integration (LVGL) is next;
the chat / scene / SFX modules follow the Phase plan in the repo history.

## Hardware Setup

Everything except audio and power rides the factory board — no hand wiring.
The speaker connects to the MX1.25 2P "Speak" terminal; USB-C handles power
and flashing.

### Pinout Reference

| Function | Signal | GPIO | Notes |
|----------|--------|------|-------|
| **RGB565 data** | R0–R4 | 11, 12, 13, 14, 0 | 16-bit parallel bus |
| | G0–G5 | 8, 20, 3, 46, 9, 10 | |
| | B0–B4 | 4, 5, 6, 7, 15 | |
| **RGB control** | DE / VSYNC / HSYNC / PCLK | 18 / 17 / 16 / 21 | PCLK 12 MHz from PLL160M |
| **Backlight** | BL | 38 | LEDC PWM |
| **Panel command SPI** | CS / SCL / SDA | 39 / 48 / 47 | 3-wire, shares SCK+MOSI with SD |
| **GT911 touch** | SDA / SCL | 19 / 45 | I²C 100 kHz, addr 0x5D |
| **Micro SD** | CS / MISO / SCK / MOSI | 42 / 41 / 48 / 47 | SDSPI @ 20 MHz |
| **I²S audio (NS4168)** | BCLK / LRCK / DOUT | 1 / 2 / 40 | desk speaker amp |

## Installation

Requires [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/) with the
Xtensa ESP32-S3 toolchain (Windows 11 + ESP-IDF PowerShell, or Linux).

```bash
git clone https://github.com/bigjoe420/KAST.git
cd KAST

idf.py set-target esp32s3   # one-time
idf.py build
idf.py -p COM3 flash monitor   # Windows — adjust the port
```

> Windows + Git Bash: `idf.py` is refused under MSYSTEM on ESP-IDF v6.1 —
> use `python idf_run.py build` / `python idf_run.py -p COM3 flash` instead.
> `idf_run.py` wraps the toolchain in a clean PowerShell subprocess and is
> location-independent.

## System Architecture

```
 Streaming PC                                          KAST (ESP32-S3)
┌──────────────────────────┐      WebSocket      ┌────────────────────────────┐
│  relay server (local)    │  ◄──────────────►   │  ui_task (LVGL 9)          │
│  Kick API auth + chat    │   chat out /        │  ws_client_task            │
│  read/send, JSON parsing │   commands in       │  touch · display · audio   │
└──────────────────────────┘                     │  SD · NVS settings         │
        │ OBS WebSocket                          └────────────────────────────┘
        └───────────────► OBS scene / media control (local network)
```

The relay layer keeps Kick credentials and heavy JSON off the microcontroller:
the ESP32 sees clean text lines and execution triggers, and sends macro /
keyboard input back through the same socket for injection into chat.

## Project Structure

```
KAST/
├── CMakeLists.txt              # Root project (target esp32s3, binary KAST.bin)
├── sdkconfig.defaults          # Mandatory Kconfig (target, flash, PSRAM, app version)
├── main/
│   ├── CMakeLists.txt          # Component registration
│   ├── main.c                  # Boot proof + subsystem bring-up entry
│   ├── pins.h                  # Canonical pin macros (board law)
│   ├── display.c/.h            # ST7701S RGB panel, vendor type1 init, backlight PWM
│   └── touch.c/.h              # GT911 driver, tap history + serial diagnostics
├── docs/
│   └── PINOUT.md               # Full pin map and board gotchas
└── idf_run.py                  # Git Bash wrapper for idf.py (MSYSTEM workaround)
```

## License

[MIT](LICENSE) — do what you like, keep the notice.

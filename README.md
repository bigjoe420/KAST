# KAST — Kick Auxiliary Streaming Terminal · Guition ESP32-S3-4848S040 (ESP32-S3)

> A streamer's desk terminal: live Kick chat with full moderation, OBS scene
> switching, a soundboard, and motion overlays — one tap each, no browser
> tabs, no second monitor.


**Board:** Guition **ESP32-S3-4848S040** — built by JCZN around the
ESP32-S3-N16R8 module (dual-core LX7 @ 240 MHz, 16 MB flash, 8 MB octal
PSRAM), with a 4.0" 480×480 **square** IPS LCD (ST7701S, 16-bit RGB
interface), GT911 capacitive touch, NS4168 I²S speaker amplifier, micro SD
slot, and USB-C (CH340 UART). The identical board is also sold under the
Sunton branding as the ESP32-4848S040, with small revision differences —
match the pinout below to your exact board. Manufacturer package:
`pan.jczn1688.com`.

KAST is a two-part system: the firmware on this board, and a small
**Companion** app that lives in the Windows system tray on the streaming PC.
The Companion owns the messy parts — Kick sign-in, OBS WebSocket, audio
playback — so the box stays a fast, quiet remote control for the stream.

## Features

| | |
|---|---|
| **Kick Chat Terminal** | Live chat over the relay link with emote rendering, a scrolling 480×480-tuned view, on-device QWERTY composer, and quick-reply macros you edit right on the box. |
| **Full moderation** | Long-press any message for delete / ban / timeout. `PANIC` sits in the status bar on every screen — one tap covers the stream, mutes the mic, stops every playing sound and graphic, and restores on the next tap. `CLEAR` drops all overlays instantly. |
| **Scene Controller** | Your own OBS scenes as full-screen tiles, in the same order as the OBS studio UI. Tap to switch; the live scene is highlighted. The Companion picker chooses which scenes become tiles — KAST never creates, renames, or deletes your scenes. |
| **SFX Soundboard** | Twelve sound slots triggered straight into OBS as managed media sources, so the audience hears exactly what you hear. Ships with 12 starter effects; add, replace, and remove sounds from the Companion's Sound Library window. |
| **Motion Graphics** | Overlay stingers (transparency-friendly QuickTime PNG/alpha media) nested into the active scene like the soundboard — tap to fire, with optional synchronized sound. Managed from the Graphics Library window, 12 slots. |
| **KAST Companion** | Zero-terminal Windows tray app: auto-discovers the box on the LAN, starts the relay, handles the firewall, walks OBS WebSocket setup, links Kick (OAuth 2.1 + PKCE — tokens stay on your PC), and hosts the Sound/Graphics libraries and scene-tile picker. |
| **First-run wizard** | Plug the box in, run the wizard: detect → flash → verify → OBS setup → Kick sign-in → Wi-Fi once on the box. Streaming without ever opening a config file. |
| **Status bar** | Wi-Fi, relay, and OBS link state at a glance, on every screen. Tap the bar for settings; Wi-Fi and relay endpoint are entered on-device and stored in NVS. |
| **Pure C firmware** | ESP-IDF v6.1, FreeRTOS task-per-subsystem, LVGL 9 on PSRAM framebuffers — static allocations in the hot path, reconnect-with-backoff on every socket, built for multi-hour streams. |

<!-- Demo GIF (boot to live chat) goes right below this line. -->

## Gallery

<!-- Owner media, EXIF-stripped. Suggested shots:
     hero — box on the desk beside the streaming rig, screen on Home
     chat — live chat page with emotes + macro row
     scenes — full-screen scene tiles with the live scene highlighted
     sfx — soundboard page with the OBS meter moving
     gfx — an overlay stinger mid-play over the program scene
     panic — status bar with PANIC engaged over the cover
     companion — tray menu + Sound Library window
     installer — first-run wizard mid-flash                        -->

## Requirements

- Windows 10/11 PC on the same network as the box (runs the Companion)
- OBS Studio 28+ — during first-time setup you'll enable its WebSocket server:
  OBS → Tools → WebSocket Server Settings → Enable → **Generate Password** →
  Apply, then *Show Connect Info* gives you the password to paste into KAST
- A Kick account you can sign into (streamer or moderator)
- Wi-Fi the box can join
- The Guition ESP32-S3-4848S040 (JCZN) board, powered over USB-C (USB-A → USB-C cable for flashing)

## Hardware Setup

Everything except the speaker rides the factory board — no hand wiring.
The desk speaker connects to the MX1.25 2P "Speak" terminal; USB-C handles
power and flashing.

### Pinout Reference

| Function | Signal | GPIO | Notes |
|----------|--------|------|-------|
| **RGB565 data** | R0–R4 | 11, 12, 13, 14, 0 | 16-bit parallel bus; GPIO0/46 strapping pins |
| | G0–G5 | 8, 20, 3, 46, 9, 10 | |
| | B0–B4 | 4, 5, 6, 7, 15 | |
| **RGB control** | DE / VSYNC / HSYNC / PCLK | 18 / 17 / 16 / 21 | PCLK 12 MHz from PLL160M |
| **Backlight** | BL | 38 | LEDC PWM |
| **Panel command SPI** | CS / SCL / SDA | 39 / 48 / 47 | 3-wire, shares SCK+MOSI with SD |
| **GT911 touch** | SDA / SCL | 19 / 45 | I²C 100 kHz, addr 0x5D, no INT/RST pins |
| **Micro SD** | CS / MISO / SCK / MOSI | 42 / 41 / 48 / 47 | SDSPI @ 20 MHz |
| **I²S audio (NS4168)** | BCLK / LRCK / DOUT | 1 / 2 / 40 | desk speaker amp |

Full pin map and board gotchas: [docs/PINOUT.md](docs/PINOUT.md).

## Installation

### Option A — KAST-Setup.exe (no toolchain needed)

Download **`KAST-Setup.exe`** from the
[latest release](https://github.com/bigjoe420/KAST/releases) and run it.
The installer bundles everything: portable Node.js for the relay, the
firmware images with a one-click flasher, and the CH340 USB driver. When it
finishes, the first-run wizard takes the box from sealed-flash to live chat.

> **The box and this PC must be on the same Wi-Fi network.** The box finds
> the PC by itself (UDP broadcast — no IP typing), and broadcast only works
> within one network. Guest or isolated networks will not do.

### Option B — build from source (ESP-IDF v6.1)

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

### PC relay

On the streaming PC, install the relay's one dependency and start it (the
Companion does this for you; this is the manual path):

```bash
cd relay
npm install
npm start            # ws://0.0.0.0:8765
npm test             # loopback smoke test
```

On the box: tap the status bar → settings → set the relay host to the PC's
LAN IP, port 8765. The status bar shows `RELAY OK` once the link is up.
Kick credentials and OBS passwords live only in the PC's gitignored
`relay/.env` — never on the device, never in the repo.

## System Architecture

```
 Streaming PC                                          KAST (ESP32-S3)
┌──────────────────────────────┐    Wi-Fi       ┌────────────────────────────┐
│  KAST Companion (tray)       │  WebSocket    │  ui_task (LVGL 9)          │
│  ├─ relay server (Node)      │ ◄──────────► │  ws_client_task            │
│  │   Kick OAuth 2.1 + PKCE   │  chat out /  │  touch · display           │
│  │   chat read/send, mod     │  commands in │  SD · NVS settings         │
│  ├─ OBS WebSocket v5 client   │              │  I²S desk speaker          │
│  └─ Sound/Graphics libraries │              └────────────────────────────┘
└──────────────┬───────────────┘
               │ OBS WebSocket (localhost)
               ▼
        OBS Studio — KAST SFX scene, KAST GFX overlay,
        KAST PANIC cover, managed media sources
```

The relay layer keeps Kick credentials, OAuth, and heavy JSON off the
microcontroller: the ESP32 sees clean text lines and triggers, and sends
macro / keyboard input back through the same socket.

## Project Structure

```
KAST/
├── CMakeLists.txt              # Root project (target esp32s3, binary KAST.bin)
├── partitions.csv              # Custom table: 3 MB factory app, NVS, coredump
├── sdkconfig.defaults          # Mandatory Kconfig (target, flash, PSRAM)
├── main/
│   ├── main.c                  # Boot staging, subsystem bring-up, app version
│   ├── pins.h                  # Canonical pin macros (board law)
│   ├── display.c/.h            # ST7701S RGB panel, vendor init, backlight PWM
│   ├── touch.c/.h              # GT911 bring-up + probe diagnostics
│   ├── ui.c/.h                 # LVGL port, chrome, screen router, status bar
│   ├── home_screen.c           # Home showcase: K-helix towers, EQ particles
│   ├── chat_screen.c           # Chat view, composer, macros, moderation cards
│   ├── scenes_screen.c         # Full-screen OBS scene tiles
│   ├── sfx_screen.c            # Soundboard triggers
│   ├── gfx_screen.c            # Motion-graphics triggers + CLEAR
│   ├── settings_screen.c       # Wi-Fi + relay entry -> NVS
│   ├── relay.c/.h              # PC-relay WebSocket client (reconnect + ping)
│   ├── net.c/.h                # Wi-Fi station, reconnect backoff, NVS creds
│   ├── audio.c/.h              # I²S desk-speaker path (NS4168)
│   ├── emote.c/.h              # Kick emote cache/render
│   ├── sd.c/.h                 # SDSPI + FATFS mount at /sdcard
│   └── config.c/.h             # NVS config layer
├── relay/                      # PC relay: server, OBS client, libraries
│   ├── server.js               # Device WS link, chat routes, Kick API client
│   ├── obs.js                  # OBS WebSocket v5 client (auth, backoff)
│   ├── obs_sfx.js / obs_gfx.js / obs_scenes.js / obs_panic.js
│   ├── sound_library.js / gfx_library.js   # 12-slot catalogs + tests
│   └── starter_sounds/         # 12 shipped effects
├── companion/                  # KAST Companion tray app (PowerShell/WPF)
│   ├── tray.ps1                # Tray host: relay, OBS setup, status
│   ├── first_run.ps1           # Guided first-run wizard
│   ├── sound_library.ps1       # Sound Library window
│   ├── graphics_library.ps1    # Graphics Library window
│   ├── scene_tiles.ps1         # Scene-tile picker
│   └── Start-KAST.ps1          # One-click stack start (OBS + relay)
├── installer/                  # Inno Setup build: KAST-Setup.exe
├── tools/                      # Serial capture + boot-log scripts
├── docs/
│   └── PINOUT.md               # Full pin map and board gotchas
└── idf_run.py                  # Git Bash wrapper for idf.py (MSYSTEM workaround)
```

## License

[MIT](LICENSE) — do what you like, keep the notice.

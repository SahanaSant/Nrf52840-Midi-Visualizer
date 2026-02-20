<<<<<<< HEAD
﻿# nRF52840 MIDI Visualizer

A personal Zephyr project that turns MIDI songs into a live EQ-style visualizer on an nRF52840 DK with an Adafruit 2.8" TFT shield.

## What it does

- Renders a 12-band animated EQ using LVGL.
- Supports two visualizer modes: vertical bars and piano mode.
- Includes multiple color scenes (`white`, `neon-night`, `botanic-pop`, `sunset-heat`, `aqua-crimson`, `rainbow-stripe` ).
- Shows song progress and a title/elapsed-time header.
- Uses precomputed MIDI frames generated from `.mid` files.
- Falls back to synthetic animation if MIDI frame data is not present.

## Hardware

- Nordic `nRF52840 DK` (`nrf52840dk/nrf52840`)
- Adafruit `2.8" TFT Touch Shield v2` (`adafruit_2_8_tft_touch_v2`)
- USB cable for power/programming

## Button controls

This app reads `sw0`..`sw3` aliases (on nRF52840 DK these map to buttons 1..4):

- `sw0`: Toggle header mode (song title <-> elapsed time)
- `sw1`: Cycle color scene
- `sw2`: Pause/resume playback
- `sw3`: Toggle visualizer mode (bars <-> piano)

## Software prerequisites

- Zephyr/nRF Connect SDK workspace with `west`
- Python 3
- Optional: virtual environment for tooling
- `Pillow` (only needed for background-image conversion)

## Quick start

From this repo root:

```powershell
west build -b nrf52840dk/nrf52840 . -d build -- -DSHIELD=adafruit_2_8_tft_touch_v2
west flash --skip-rebuild -d build
```

Optional debug config:

```powershell
west build -b nrf52840dk/nrf52840 . -d build -- -DSHIELD=adafruit_2_8_tft_touch_v2 -DOVERLAY_CONFIG=debug.conf
```

## Import a MIDI song

Generate `src/midi_eq_data.c` + `src/midi_eq_data.h` from a MIDI file:

```powershell
python tools/convert_midi_to_eq.py "C:\path\to\song.mid" --mapping balanced --drum-scale 0.45 --frame-ms 30 --bar-count 12 --balance-smoothing 0.25 --print-stats --out-dir src
```

Then rebuild/flash.

### One-command workflow

This helper converts MIDI, builds, and flashes in one step:

```powershell
python tools/load_midi.py "C:\path\to\song.mid" --print-stats
```

Useful flags:

- `--no-build`: only regenerate MIDI EQ data
- `--no-flash`: convert + build, skip flashing
- `--dev-id <id>`: select a specific debug probe

## Optional background image

Convert an image into LVGL C assets (`src/bg_image.c` / `src/bg_image.h`):

```powershell
python -m pip install pillow
python tools/convert_bg_image.py "C:\path\to\image.jpg" --width 320 --height 240 --out-dir src
```

If `bg_image` files are missing, the app uses a generated gradient/blob background.

# Project Structure (What Each File Is For)

## `Nrf52840-Midi-Visualizer/`
-   `boards/`
-     `nrf52840dk_nrf52840.overlay`
- Devicetree overlay for board/shield pin/device config overrides used by this app.
##  `src/`
-     `main.c`
- Main firmware app: LVGL UI creation, button handling, mode switching (EQ/piano), and render/update loops.
-     `midi_eq_data.h`
- Auto-generated constants for MIDI frame playback (MIDI_EQ_BAR_COUNT, MIDI_EQ_FRAME_MS, MIDI_EQ_FRAME_COUNT, title).
-     `midi_eq_data.c`
- Auto-generated per-frame EQ data array used at runtime to drive animation.
-     `bg_image.h`
- Declaration for converted LVGL background image descriptor
-     `bg_image.c`
- Converted LVGL image bytes/descriptor used as background asset.
##  `tools/`
-     `load_midi.py`
- One-command helper to convert MIDI, then build and optionally flash. 
-     `convert_midi_to_eq.py`
- Converts .mid into firmware-friendly frame data (midi_eq_data.h/.c).
-     `convert_bg_image.py`
- Converts regular image files into LVGL ARGB image C source (bg_image.h/.c).
- ## `(other, etc)`
-   prj.conf
-   CMakeLists.txt
-   Kconfig
-   sample.yaml
-   VERSION
-   debug.conf

## Troubleshooting

- `No zephyr,display chosen node in devicetree`: check board/shield build flags.
- `Display device not ready`: confirm shield wiring/mounting and correct `-DSHIELD=...` value.
- Buttons not responding: verify your board exposes `sw0`..`sw3` aliases.

# Notes

This is a personal project and an active playground for visual tuning, MIDI mapping, and UI effects.
EQ mode and piano mode are mutually exclusive on screen.
UI scrolling is disabled for a fixed display.
Piano mode currently maps from generated 12-band MIDI frame data (not raw per-note event stream yet).
In white scene, piano keys are classic black/white and hit notes flash yellow.

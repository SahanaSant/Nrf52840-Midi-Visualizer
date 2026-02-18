# nRF52840 MIDI Visualizer

A personal Zephyr project that turns MIDI songs into a live EQ-style visualizer on an nRF52840 DK with an Adafruit 2.8" TFT shield.

## What it does

- Renders a 12-band animated EQ using LVGL.
- Supports two visualizer modes: vertical bars and circular radial lines.
- Includes multiple color scenes (`white`, `neon-night`, `botanic-pop`, `sunset-heat`).
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
- `sw3`: Toggle visualizer mode (bars <-> circle)

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

## Project structure

- `src/main.c`: Visualizer UI, animation engine, button handling
- `src/midi_eq_data.*`: Generated song frame data
- `src/bg_image.*`: Optional generated background image asset
- `boards/nrf52840dk_nrf52840.overlay`: Board/shield/pin configuration
- `tools/convert_midi_to_eq.py`: MIDI -> EQ frame converter
- `tools/load_midi.py`: Convert + build + flash helper
- `tools/convert_bg_image.py`: Image -> LVGL asset converter

## Troubleshooting

- `No zephyr,display chosen node in devicetree`: check board/shield build flags.
- `Display device not ready`: confirm shield wiring/mounting and correct `-DSHIELD=...` value.
- Buttons not responding: verify your board exposes `sw0`..`sw3` aliases.

## Notes

This is a personal project and an active playground for visual tuning, MIDI mapping, and UI effects.

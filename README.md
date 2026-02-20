# NRF52840 MIDI Visualizer

LVGL visualizer app for `nrf52840dk/nrf52840` with the `adafruit_2_8_tft_touch_v2` shield.

It currently supports:
- EQ bar mode
- Piano mode
- Palette switching
- MIDI-imported animation data

## Current Controls

- `SW0` (`button 1`): toggle header between song title and elapsed time.
- `SW1` (`button 2`): cycle color scenes/palettes.
- `SW2` (`button 3`): pause/resume playback animation.
- `SW3` (`button 4`): toggle `EQ mode` <-> `Piano mode`.

Notes:
- UI scrolling is disabled (fixed screen).
- In piano mode, EQ bars are hidden.
- In white scene, piano keys are black/white and hit notes flash yellow.

## Build And Flash

From the project root (`Nrf52840-Midi-Visualizer`):

```powershell
west build -b nrf52840dk/nrf52840 . -d build -- -DSHIELD=adafruit_2_8_tft_touch_v2
west flash --skip-rebuild -d build --dev-id <JLINK_SERIAL>
```

If you do not need a specific probe ID, omit `--dev-id`.

## MIDI Pipeline

### One-command load (convert + build + flash)

```powershell
python tools/load_midi.py "C:\path\to\song.mid" --print-stats --dev-id <JLINK_SERIAL>
```

Useful options:
- `--no-build`
- `--no-flash`
- `--mapping balanced|linear`
- `--drum-scale 0.45`
- `--balance-smoothing 0.25`
- `--frame-ms 20`

### Convert only (advanced)

```powershell
python tools/convert_midi_to_eq.py "C:\path\to\song.mid" --mapping balanced --drum-scale 0.45 --balance-smoothing 0.25 --frame-ms 20 --bar-count 12 --print-stats --out-dir src
```

This generates:
- `src/midi_eq_data.h`
- `src/midi_eq_data.c`

## Piano Mode Mapping (Current)

Current piano mapping uses generated `midi_eq_frames` (12-band data), not raw per-note events yet.

- Per frame, strongest bands are selected.
- Those bands map to piano key positions.
- Visuals are limited to at most 2-3 highlighted keys at a time.

## Background Image Conversion

```powershell
python tools/convert_bg_image.py "C:\path\to\image.jpg" --width 320 --height 240 --out-dir src
```

This generates:
- `src/bg_image.h`
- `src/bg_image.c`

## Dependencies

- Zephyr + west toolchain
- Python 3
- Pillow (only needed for image conversion):

```powershell
pip install pillow
```

## Project Layout

- `src/main.c`: UI, button handling, EQ and piano render loops.
- `src/midi_eq_data.*`: generated animation frames from MIDI.
- `src/bg_image.*`: generated LVGL image asset.
- `tools/convert_midi_to_eq.py`: MIDI to frame-data generator.
- `tools/load_midi.py`: helper to convert/build/flash in one command.
- `tools/convert_bg_image.py`: image to LVGL ARGB asset generator.

#pragma once
#include <stdint.h>

#define MIDI_EQ_BAR_COUNT 12
#define MIDI_EQ_FRAME_MS 30
#define MIDI_EQ_FRAME_COUNT 290
#define MIDI_EQ_TITLE "Coldplay - Viva La Vida"

extern const uint8_t midi_eq_frames[MIDI_EQ_FRAME_COUNT][MIDI_EQ_BAR_COUNT];

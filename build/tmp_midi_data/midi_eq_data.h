#pragma once
#include <stdint.h>

#define MIDI_EQ_BAR_COUNT 12
#define MIDI_EQ_FRAME_MS 30
#define MIDI_EQ_FRAME_COUNT 10805

extern const uint8_t midi_eq_frames[MIDI_EQ_FRAME_COUNT][MIDI_EQ_BAR_COUNT];

#pragma once
#include <stdint.h>

#define MIDI_EQ_BAR_COUNT 12
#define MIDI_EQ_FRAME_MS 20
#define MIDI_EQ_FRAME_COUNT 16207
#define MIDI_EQ_TITLE "Billie Jean 3"

extern const uint8_t midi_eq_frames[MIDI_EQ_FRAME_COUNT][MIDI_EQ_BAR_COUNT];

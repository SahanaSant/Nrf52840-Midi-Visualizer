#!/usr/bin/env python3
"""Convert a MIDI file into precomputed EQ bar frames for firmware playback."""

from __future__ import annotations

import argparse
import math
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass
class MidiEvent:
    tick: int
    seq: int
    kind: str
    note: int = 0
    vel: int = 0
    channel: int = 0
    tempo: int = 500_000
    ms: float = 0.0


@dataclass
class SynthesisStats:
    note_on_hits: list[int]
    note_on_vel_sum: list[int]
    active_frames: list[int]
    frame_count: int
    baseline: int


def sanitize_title(raw: str) -> str:
    text = raw.replace("_", " ")
    safe = []
    for ch in text:
        code = ord(ch)
        if 32 <= code < 127:
            safe.append(ch)
        else:
            safe.append(" ")
    compact = " ".join("".join(safe).split())
    if not compact:
        return "midi eq"
    return compact[:60]


def c_escape_string(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


def read_vlq(buf: bytes, offset: int) -> tuple[int, int]:
    value = 0
    while True:
        b = buf[offset]
        offset += 1
        value = (value << 7) | (b & 0x7F)
        if (b & 0x80) == 0:
            break
    return value, offset


def parse_midi_file(path: Path) -> tuple[int, list[bytes]]:
    data = path.read_bytes()
    if data[0:4] != b"MThd":
        raise ValueError("Invalid MIDI header")

    header_len = struct.unpack(">I", data[4:8])[0]
    if header_len < 6:
        raise ValueError("MIDI header too short")

    _fmt, track_count, division = struct.unpack(">HHH", data[8:14])
    if division & 0x8000:
        raise ValueError("SMPTE time division is not supported")

    off = 8 + header_len
    tracks: list[bytes] = []
    for _ in range(track_count):
        if data[off : off + 4] != b"MTrk":
            raise ValueError("Missing track chunk")
        trk_len = struct.unpack(">I", data[off + 4 : off + 8])[0]
        start = off + 8
        end = start + trk_len
        tracks.append(data[start:end])
        off = end

    return division, tracks


def parse_tracks(tracks: list[bytes]) -> list[MidiEvent]:
    events: list[MidiEvent] = []

    for track_index, trk in enumerate(tracks):
        off = 0
        tick = 0
        running_status: int | None = None
        local_seq = 0

        while off < len(trk):
            delta, off = read_vlq(trk, off)
            tick += delta

            status = trk[off]
            if status == 0xFF:  # Meta event
                off += 1
                meta_type = trk[off]
                off += 1
                length, off = read_vlq(trk, off)
                payload = trk[off : off + length]
                off += length

                if meta_type == 0x51 and length == 3:
                    tempo = int.from_bytes(payload, "big")
                    events.append(
                        MidiEvent(
                            tick=tick,
                            seq=(track_index << 24) + local_seq,
                            kind="tempo",
                            tempo=tempo,
                        )
                    )

                if meta_type == 0x2F:  # End of track
                    break

                running_status = None
                local_seq += 1
                continue

            if status in (0xF0, 0xF7):  # SysEx event
                off += 1
                length, off = read_vlq(trk, off)
                off += length
                running_status = None
                local_seq += 1
                continue

            if status & 0x80:
                off += 1
                running_status = status
            elif running_status is not None:
                status = running_status
            else:
                raise ValueError("Running status used before status byte")

            ev_type = status & 0xF0
            channel = status & 0x0F
            if ev_type in (0xC0, 0xD0):
                off += 1
                local_seq += 1
                continue

            d1 = trk[off]
            d2 = trk[off + 1]
            off += 2

            if ev_type == 0x90:
                kind = "note_off" if d2 == 0 else "note_on"
                events.append(
                    MidiEvent(
                        tick=tick,
                        seq=(track_index << 24) + local_seq,
                        kind=kind,
                        note=d1,
                        vel=d2,
                        channel=channel,
                    )
                )
            elif ev_type == 0x80:
                events.append(
                    MidiEvent(
                        tick=tick,
                        seq=(track_index << 24) + local_seq,
                        kind="note_off",
                        note=d1,
                        vel=d2,
                        channel=channel,
                    )
                )

            local_seq += 1

    events.sort(key=lambda e: (e.tick, e.seq))
    return events


def apply_timing(events: list[MidiEvent], ticks_per_quarter: int) -> float:
    tempo = 500_000  # us per quarter note
    last_tick = 0
    elapsed_ms = 0.0

    for ev in events:
        dt = ev.tick - last_tick
        if dt > 0:
            elapsed_ms += (dt * tempo) / ticks_per_quarter / 1000.0
            last_tick = ev.tick

        if ev.kind == "tempo":
            tempo = ev.tempo
        else:
            ev.ms = elapsed_ms

    return elapsed_ms


def map_note_to_bar(note: int, note_min: int, note_max: int, bar_count: int) -> int:
    """Linear note-to-bar mapping kept for compatibility with existing tools."""
    span = max(1, note_max - note_min + 1)
    idx = ((note - note_min) * bar_count) // span
    if idx < 0:
        return 0
    if idx >= bar_count:
        return bar_count - 1
    return idx


def build_weighted_histogram(
    note_on_events: list[MidiEvent],
    note_min: int,
    note_max: int,
    drum_scale: float,
) -> list[float]:
    span = max(1, note_max - note_min + 1)
    hist = [0.0 for _ in range(span)]

    for ev in note_on_events:
        idx = ev.note - note_min
        if idx < 0 or idx >= span:
            continue
        vel_w = 0.35 + (0.65 * (ev.vel / 127.0))
        if ev.channel == 9:  # MIDI channel 10 (1-based) = drums
            vel_w *= drum_scale
        hist[idx] += vel_w

    return hist


def build_balanced_lookup(hist: list[float], bar_count: int) -> list[int]:
    span = len(hist)
    if span <= 1:
        return [0]

    total = sum(hist)
    if total <= 0.0:
        return [min(bar_count - 1, (i * bar_count) // span) for i in range(span)]

    boundaries = [0]  # start index for each bar
    cumulative = 0.0
    target_idx = 1

    for note_idx, w in enumerate(hist):
        cumulative += w
        while target_idx < bar_count and cumulative >= (total * target_idx / bar_count):
            start = note_idx + 1
            if start >= span:
                start = span - 1
            boundaries.append(start)
            target_idx += 1

    while len(boundaries) < bar_count:
        boundaries.append(span - 1)

    for i in range(1, len(boundaries)):
        if boundaries[i] < boundaries[i - 1]:
            boundaries[i] = boundaries[i - 1]

    lookup = [0 for _ in range(span)]
    current_bar = 0
    for note_idx in range(span):
        while current_bar + 1 < bar_count and note_idx >= boundaries[current_bar + 1]:
            current_bar += 1
        lookup[note_idx] = current_bar

    return lookup


def build_note_to_bar_lookup(
    note_events: list[MidiEvent],
    bar_count: int,
    mapping: str,
    drum_scale: float,
) -> tuple[int, list[int]]:
    note_on_events = [ev for ev in note_events if ev.kind == "note_on"]
    if not note_on_events:
        return 0, [0]

    note_min = min(ev.note for ev in note_on_events)
    note_max = max(ev.note for ev in note_on_events)
    span = max(1, note_max - note_min + 1)

    if mapping == "linear":
        lookup = [map_note_to_bar(note_min + i, note_min, note_max, bar_count) for i in range(span)]
        return note_min, lookup

    hist = build_weighted_histogram(note_on_events, note_min, note_max, drum_scale)
    lookup = build_balanced_lookup(hist, bar_count)
    return note_min, lookup


def map_note_with_lookup(note: int, note_min: int, lookup: list[int]) -> int:
    idx = note - note_min
    if idx < 0:
        return lookup[0]
    if idx >= len(lookup):
        return lookup[-1]
    return lookup[idx]


def synthesize_frames(
    note_events: list[MidiEvent],
    bar_count: int,
    frame_ms: int,
    baseline: int,
    peak_span: int,
    mapping: str,
    drum_scale: float,
    balance_smoothing: float,
) -> tuple[list[list[int]], SynthesisStats]:
    if not note_events:
        frames = [[baseline for _ in range(bar_count)]]
        stats = SynthesisStats(
            note_on_hits=[0 for _ in range(bar_count)],
            note_on_vel_sum=[0 for _ in range(bar_count)],
            active_frames=[0 for _ in range(bar_count)],
            frame_count=1,
            baseline=baseline,
        )
        return frames, stats

    note_min, lookup = build_note_to_bar_lookup(
        note_events=note_events,
        bar_count=bar_count,
        mapping=mapping,
        drum_scale=drum_scale,
    )

    mapped_note_on: list[tuple[float, int, int, int]] = []
    note_on_hits = [0 for _ in range(bar_count)]
    note_on_vel_sum = [0 for _ in range(bar_count)]

    for ev in note_events:
        if ev.kind != "note_on":
            continue
        b = map_note_with_lookup(ev.note, note_min, lookup)
        mapped_note_on.append((ev.ms, b, ev.vel, ev.channel))
        note_on_hits[b] += 1
        note_on_vel_sum[b] += ev.vel

    if not mapped_note_on:
        frames = [[baseline for _ in range(bar_count)]]
        stats = SynthesisStats(
            note_on_hits=note_on_hits,
            note_on_vel_sum=note_on_vel_sum,
            active_frames=[0 for _ in range(bar_count)],
            frame_count=1,
            baseline=baseline,
        )
        return frames, stats

    tail_ms = 1800.0
    end_ms = max(ms for ms, _, _, _ in mapped_note_on) + tail_ms
    frame_count = max(1, int(math.ceil(end_ms / frame_ms)))

    decay = math.exp(-(frame_ms / 1000.0) * 3.2)
    energies = [0.0 for _ in range(bar_count)]
    frames: list[list[int]] = []
    active_frames = [0 for _ in range(bar_count)]

    event_index = 0
    for fi in range(frame_count):
        t_ms = fi * frame_ms

        while event_index < len(mapped_note_on) and mapped_note_on[event_index][0] <= t_ms:
            _, b, vel, ch = mapped_note_on[event_index]
            v = vel / 127.0
            if ch == 9:
                v *= drum_scale

            energies[b] += 1.25 * v

            spread = balance_smoothing * v
            if spread > 0.0:
                if b > 0:
                    energies[b - 1] += spread
                if b + 1 < bar_count:
                    energies[b + 1] += spread
                edge_spread = spread * 0.35
                if b > 1:
                    energies[b - 2] += edge_spread
                if b + 2 < bar_count:
                    energies[b + 2] += edge_spread

            event_index += 1

        for i in range(bar_count):
            energies[i] *= decay
            if energies[i] < 0.002:
                energies[i] = 0.0

        row: list[int] = []
        for i in range(bar_count):
            x = energies[i]
            norm = math.tanh(x * 1.1)
            lvl = baseline + int(round(norm * peak_span))
            if lvl < 0:
                lvl = 0
            if lvl > 100:
                lvl = 100
            row.append(lvl)
            if lvl > (baseline + 2):
                active_frames[i] += 1
        frames.append(row)

    stats = SynthesisStats(
        note_on_hits=note_on_hits,
        note_on_vel_sum=note_on_vel_sum,
        active_frames=active_frames,
        frame_count=frame_count,
        baseline=baseline,
    )
    return frames, stats


def write_header(path: Path, bar_count: int, frame_ms: int, frame_count: int, title: str) -> None:
    title_escaped = c_escape_string(title)
    text = f"""#pragma once
#include <stdint.h>

#define MIDI_EQ_BAR_COUNT {bar_count}
#define MIDI_EQ_FRAME_MS {frame_ms}
#define MIDI_EQ_FRAME_COUNT {frame_count}
#define MIDI_EQ_TITLE "{title_escaped}"

extern const uint8_t midi_eq_frames[MIDI_EQ_FRAME_COUNT][MIDI_EQ_BAR_COUNT];
"""
    path.write_text(text, encoding="utf-8")


def write_source(path: Path, frames: list[list[int]]) -> None:
    lines = [
        '#include "midi_eq_data.h"',
        "",
        "const uint8_t midi_eq_frames[MIDI_EQ_FRAME_COUNT][MIDI_EQ_BAR_COUNT] = {",
    ]
    for row in frames:
        vals = ", ".join(f"{v:3d}" for v in row)
        lines.append(f"    {{ {vals} }},")
    lines.append("};")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def print_stats(stats: SynthesisStats) -> None:
    print("Per-bar MIDI and frame activity stats:")
    threshold = stats.baseline + 2
    for i, hits in enumerate(stats.note_on_hits):
        avg_vel = 0.0
        if hits > 0:
            avg_vel = stats.note_on_vel_sum[i] / hits
        active = stats.active_frames[i]
        active_ratio = active / max(1, stats.frame_count)
        line = (
            f"bar{i + 1:02d}: hits={hits:5d} avg_vel={avg_vel:6.2f} "
            f"active_frames(>{threshold})={active:5d} ratio={active_ratio:6.2%}"
        )
        if active_ratio < 0.03:
            line += "  WARNING: below 3% activity"
        print(line)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("midi", type=Path, help="Input .mid file")
    parser.add_argument("--bar-count", type=int, default=12)
    parser.add_argument("--frame-ms", type=int, default=20)
    parser.add_argument("--baseline", type=int, default=33)
    parser.add_argument("--peak-span", type=int, default=67)
    parser.add_argument("--mapping", choices=("linear", "balanced"), default="balanced")
    parser.add_argument("--drum-scale", type=float, default=0.45)
    parser.add_argument("--balance-smoothing", type=float, default=0.25)
    parser.add_argument("--title", type=str, default=None, help="Optional on-screen title override")
    parser.add_argument("--print-stats", action="store_true")
    parser.add_argument("--out-dir", type=Path, default=Path("src"))
    args = parser.parse_args()

    if not args.midi.exists():
        raise SystemExit(f"Input file not found: {args.midi}")
    if args.bar_count <= 0:
        raise SystemExit("bar-count must be > 0")
    if args.frame_ms <= 0:
        raise SystemExit("frame-ms must be > 0")
    if not (0.0 <= args.drum_scale <= 2.0):
        raise SystemExit("drum-scale must be in [0.0, 2.0]")
    if not (0.0 <= args.balance_smoothing <= 1.0):
        raise SystemExit("balance-smoothing must be in [0.0, 1.0]")

    tpq, tracks = parse_midi_file(args.midi)
    events = parse_tracks(tracks)
    _song_len_ms = apply_timing(events, tpq)
    note_events = [e for e in events if e.kind in ("note_on", "note_off")]

    frames, stats = synthesize_frames(
        note_events=note_events,
        bar_count=args.bar_count,
        frame_ms=args.frame_ms,
        baseline=args.baseline,
        peak_span=args.peak_span,
        mapping=args.mapping,
        drum_scale=args.drum_scale,
        balance_smoothing=args.balance_smoothing,
    )

    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)
    title = sanitize_title(args.title if args.title is not None else args.midi.stem)

    write_header(out_dir / "midi_eq_data.h", args.bar_count, args.frame_ms, len(frames), title)
    write_source(out_dir / "midi_eq_data.c", frames)

    print(f"Wrote {out_dir / 'midi_eq_data.h'}")
    print(f"Wrote {out_dir / 'midi_eq_data.c'}")
    print(f"Frames: {len(frames)} @ {args.frame_ms} ms")
    print(f"Title: {title}")
    print(f"Mapping: {args.mapping}, drum-scale: {args.drum_scale}, balance-smoothing: {args.balance_smoothing}")
    if args.print_stats:
        print_stats(stats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""One-command MIDI import: convert MIDI, build, and flash."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], cwd: Path) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=str(cwd), check=True)


def default_west(project_root: Path) -> str:
    local = project_root.parent / ".venv" / "Scripts" / "west.exe"
    if local.exists():
        return str(local)
    return "west"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("midi", type=Path, help="Input MIDI file")
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--out-dir", type=Path, default=None, help="Defaults to <project-root>/src")
    parser.add_argument("--mapping", choices=("linear", "balanced"), default="balanced")
    parser.add_argument("--drum-scale", type=float, default=0.45)
    parser.add_argument("--balance-smoothing", type=float, default=0.25)
    parser.add_argument("--bar-count", type=int, default=12)
    parser.add_argument("--frame-ms", type=int, default=20)
    parser.add_argument("--baseline", type=int, default=33)
    parser.add_argument("--peak-span", type=int, default=67)
    parser.add_argument("--title", type=str, default=None, help="Optional title override")
    parser.add_argument("--print-stats", action="store_true")
    parser.add_argument("--no-build", action="store_true")
    parser.add_argument("--no-flash", action="store_true")
    parser.add_argument("--board", default="nrf52840dk/nrf52840")
    parser.add_argument("--shield", default="adafruit_2_8_tft_touch_v2")
    parser.add_argument("--build-dir", default="build")
    parser.add_argument("--dev-id", default=None)
    parser.add_argument("--west", default=None)
    args = parser.parse_args()

    project_root = args.project_root.resolve()
    out_dir = args.out_dir.resolve() if args.out_dir else (project_root / "src")
    midi = args.midi.resolve()
    if not midi.exists():
        raise SystemExit(f"MIDI file not found: {midi}")

    converter = project_root / "tools" / "convert_midi_to_eq.py"
    west = args.west or default_west(project_root)

    convert_cmd = [
        sys.executable,
        str(converter),
        str(midi),
        "--mapping",
        args.mapping,
        "--drum-scale",
        str(args.drum_scale),
        "--balance-smoothing",
        str(args.balance_smoothing),
        "--bar-count",
        str(args.bar_count),
        "--frame-ms",
        str(args.frame_ms),
        "--baseline",
        str(args.baseline),
        "--peak-span",
        str(args.peak_span),
        "--out-dir",
        str(out_dir),
    ]
    if args.title:
        convert_cmd.extend(["--title", args.title])
    if args.print_stats:
        convert_cmd.append("--print-stats")

    run(convert_cmd, cwd=project_root.parent)

    if args.no_build:
        return 0

    build_cmd = [
        west,
        "build",
        "-b",
        args.board,
        str(project_root),
        "-d",
        str(project_root / args.build_dir),
        "--",
        f"-DSHIELD={args.shield}",
    ]
    run(build_cmd, cwd=project_root.parent)

    if args.no_flash:
        return 0

    flash_cmd = [
        west,
        "flash",
        "--skip-rebuild",
        "-d",
        str(project_root / args.build_dir),
    ]
    if args.dev_id:
        flash_cmd.extend(["--dev-id", args.dev_id])
    run(flash_cmd, cwd=project_root.parent)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

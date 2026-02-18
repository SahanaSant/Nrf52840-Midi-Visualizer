#!/usr/bin/env python3
"""Convert a regular image into an LVGL ARGB8888 C asset for this app."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image


def center_crop_resize(img: Image.Image, out_w: int, out_h: int) -> Image.Image:
    src_w, src_h = img.size
    src_ratio = src_w / src_h
    out_ratio = out_w / out_h

    if src_ratio > out_ratio:
        new_h = src_h
        new_w = int(src_h * out_ratio)
        left = (src_w - new_w) // 2
        top = 0
    else:
        new_w = src_w
        new_h = int(src_w / out_ratio)
        left = 0
        top = (src_h - new_h) // 2

    cropped = img.crop((left, top, left + new_w, top + new_h))
    return cropped.resize((out_w, out_h), Image.Resampling.LANCZOS)


def bytes_to_c_array(data: bytes) -> str:
    lines = []
    row = []
    for i, b in enumerate(data, start=1):
        row.append(f"0x{b:02X}")
        if i % 16 == 0:
            lines.append("    " + ", ".join(row) + ",")
            row = []
    if row:
        lines.append("    " + ", ".join(row) + ",")
    return "\n".join(lines)


def write_header(path: Path) -> None:
    text = """#pragma once
#include <lvgl.h>

extern const lv_image_dsc_t bg_image;
"""
    path.write_text(text, encoding="utf-8")


def write_source(path: Path, rgba: bytes, width: int, height: int) -> None:
    array_body = bytes_to_c_array(rgba)
    stride = width * 4

    text = f"""#include \"bg_image.h\"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMAGE_BG_IMAGE
#define LV_ATTRIBUTE_IMAGE_BG_IMAGE
#endif

const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMAGE_BG_IMAGE
uint8_t bg_image_map[] = {{
{array_body}
}};

const lv_image_dsc_t bg_image = {{
    .header = {{
        .magic = LV_IMAGE_HEADER_MAGIC,
        .cf = LV_COLOR_FORMAT_ARGB8888,
        .w = {width},
        .h = {height},
        .stride = {stride},
    }},
    .data_size = sizeof(bg_image_map),
    .data = bg_image_map,
}};
"""

    path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path, help="Input JPG/PNG path")
    parser.add_argument("--width", type=int, default=320)
    parser.add_argument("--height", type=int, default=240)
    parser.add_argument("--out-dir", type=Path, default=Path("src"))
    args = parser.parse_args()

    if not args.image.exists():
        raise SystemExit(f"Input file not found: {args.image}")

    img = Image.open(args.image).convert("RGBA")
    img = center_crop_resize(img, args.width, args.height)
    rgba = img.tobytes("raw", "RGBA")

    out_dir = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    write_header(out_dir / "bg_image.h")
    write_source(out_dir / "bg_image.c", rgba, args.width, args.height)

    print(f"Wrote {out_dir / 'bg_image.h'}")
    print(f"Wrote {out_dir / 'bg_image.c'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

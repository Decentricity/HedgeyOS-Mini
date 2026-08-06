#!/usr/bin/env python3
"""Generate the compact monochrome HedgeyOS Mini home-screen wordmark."""

from pathlib import Path
import zlib

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
FONT = Path("/usr/share/fonts/opentype/urw-base35/Z003-MediumItalic.otf")
OUTPUT = ROOT / "lib/Images/hedgeyos_mini.h"
PREVIEW = Path("/tmp/hedgeyos_mini.png")
TEXT = "HedgeyOS Mini"
FONT_SIZE = 85
PADDING = 4


def main() -> None:
    font = ImageFont.truetype(str(FONT), FONT_SIZE)
    canvas = Image.new("L", (900, 180), 255)
    draw = ImageDraw.Draw(canvas)
    draw.text((20, 10), TEXT, font=font, fill=0, stroke_width=2, stroke_fill=0)
    bounds = canvas.point(lambda value: 255 - value).getbbox()
    if bounds is None:
        raise RuntimeError("wordmark rendered as an empty image")
    left, top, right, bottom = bounds
    logo = canvas.crop((left - PADDING, top - PADDING, right + PADDING, bottom + PADDING))
    logo = logo.point(lambda value: 255 if value < 160 else 0, mode="1")
    logo.save(PREVIEW)

    width, height = logo.size
    stride = (width + 7) // 8
    pixels = logo.load()
    packed = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            if pixels[x, y]:
                packed[y * stride + x // 8] |= 1 << (x % 8)

    compressor = zlib.compressobj(level=9, wbits=-15)
    compressed = compressor.compress(bytes(packed)) + compressor.flush()
    rows = []
    for offset in range(0, len(compressed), 16):
        rows.append("  " + ", ".join(f"0x{value:02x}" for value in compressed[offset:offset + 16]))

    OUTPUT.write_text(
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"const uint16_t hedgeyos_mini_width = {width};\n"
        f"const uint16_t hedgeyos_mini_height = {height};\n"
        f"const uint16_t hedgeyos_mini_stride = {stride};\n"
        f"const uint32_t hedgeyos_mini_raw_size = {len(packed)};\n"
        "const uint8_t hedgeyos_mini_data[] = {\n"
        + ",\n".join(rows)
        + "\n};\n",
        encoding="utf-8",
    )
    print(f"Generated {width}x{height} logo: {len(packed)} raw bytes, {len(compressed)} compressed")


if __name__ == "__main__":
    main()

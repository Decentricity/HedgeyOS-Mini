#!/usr/bin/env python3
"""Generate compact monochrome assets for the HedgeyOS Mini home screen."""

from pathlib import Path
import zlib

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
TITLE_FONT = ROOT / "scripts/fonts/Pacifico-Regular.ttf"
HEDGEHOG_SOURCE = ROOT / "scripts/assets/hedgehog.png"
TITLE_OUTPUT = ROOT / "lib/Images/hedgeyos_mini.h"
HEDGEHOG_OUTPUT = ROOT / "lib/Images/hedgehog.h"


def write_monochrome_asset(image: Image.Image, output: Path, prefix: str) -> None:
    width, height = image.size
    stride = (width + 7) // 8
    pixels = image.load()
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

    output.write_text(
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"const uint16_t {prefix}_width = {width};\n"
        f"const uint16_t {prefix}_height = {height};\n"
        f"const uint16_t {prefix}_stride = {stride};\n"
        f"const uint32_t {prefix}_raw_size = {len(packed)};\n"
        f"const uint8_t {prefix}_data[] = {{\n"
        + ",\n".join(rows)
        + "\n};\n",
        encoding="utf-8",
    )
    print(f"Generated {prefix} {width}x{height}: {len(packed)} raw, {len(compressed)} compressed")


def write_grayscale_asset(image: Image.Image, output: Path, prefix: str) -> None:
    width, height = image.size
    pixels = image.load()
    packed = bytearray((width * height + 1) // 2)
    for y in range(height):
        for x in range(width):
            pixel_index = y * width + x
            gray = round(pixels[x, y] / 17)
            if pixel_index % 2 == 0:
                packed[pixel_index // 2] = gray << 4
            else:
                packed[pixel_index // 2] |= gray

    compressor = zlib.compressobj(level=9, wbits=-15)
    compressed = compressor.compress(bytes(packed)) + compressor.flush()
    rows = []
    for offset in range(0, len(compressed), 16):
        rows.append("  " + ", ".join(f"0x{value:02x}" for value in compressed[offset:offset + 16]))
    output.write_text(
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"const uint16_t {prefix}_width = {width};\n"
        f"const uint16_t {prefix}_height = {height};\n"
        f"const uint32_t {prefix}_raw_size = {len(packed)};\n"
        f"const uint8_t {prefix}_data[] = {{\n"
        + ",\n".join(rows)
        + "\n};\n",
        encoding="utf-8",
    )
    print(f"Generated {prefix} {width}x{height}: {len(packed)} raw, {len(compressed)} compressed")


def generate_title() -> None:
    font = ImageFont.truetype(str(TITLE_FONT), 67)
    canvas = Image.new("L", (700, 160), 255)
    draw = ImageDraw.Draw(canvas)
    draw.text((12, 0), "HedgeyOS Mini", font=font, fill=0, stroke_width=1, stroke_fill=0)
    bounds = canvas.point(lambda value: 255 - value).getbbox()
    if bounds is None:
        raise RuntimeError("wordmark rendered as an empty image")
    left, top, right, bottom = bounds
    title = canvas.crop((left - 4, top - 4, right + 4, bottom + 4))
    title = title.point(lambda value: 255 if value < 160 else 0, mode="1")
    title.save("/tmp/hedgeyos_mini.png")
    write_monochrome_asset(title, TITLE_OUTPUT, "hedgeyos_mini")


def generate_hedgehog() -> None:
    source = Image.open(HEDGEHOG_SOURCE).convert("RGBA")
    bounds = source.getchannel("A").getbbox()
    if bounds is None:
        raise RuntimeError("hedgehog source image is empty")
    left, top, right, bottom = bounds
    padding = 6
    source = source.crop((max(0, left - padding), max(0, top - padding),
                          min(source.width, right + padding), min(source.height, bottom + padding)))
    scale = min(180 / source.width, 280 / source.height)
    source = source.resize((round(source.width * scale), round(source.height * scale)),
                           Image.Resampling.LANCZOS)
    background = Image.new("RGBA", source.size, "white")
    background.alpha_composite(source)
    hedgehog = background.convert("L").point(lambda value: round(value / 17) * 17)
    hedgehog.save("/tmp/hedgehog.png")
    write_grayscale_asset(hedgehog, HEDGEHOG_OUTPUT, "hedgehog")


if __name__ == "__main__":
    generate_title()
    generate_hedgehog()

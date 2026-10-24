#!/usr/bin/env python3
"""Embed the captured current S2 terrain scene as a compact native image."""

from __future__ import annotations

import argparse
import hashlib
from io import BytesIO
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_PATH = ROOT / "assets/textures/loading_backdrop/seed42-relief.png"
OUTPUT_PATH = ROOT / "src/client/include/client/LoadingBackdrop.hpp"
SOURCE_SHA256 = "4cd964a6231cb754a85fda69708a2e2686182c5485f3957a3bece3ea7256ce50"
WIDTH = 512
HEIGHT = 288
PALETTE_SIZE = 256
INDEX_CHUNK_LENGTH = 112


def shade_alpha(vertical_position: float) -> float:
    stops = ((0.0, 0.50), (0.48, 0.12), (0.68, 0.04), (1.0, 0.66))
    for (start_y, start_alpha), (end_y, end_alpha) in zip(stops, stops[1:]):
        if vertical_position <= end_y:
            amount = (vertical_position - start_y) / (end_y - start_y)
            return start_alpha + (end_alpha - start_alpha) * amount
    return stops[-1][1]


def prepare_pixels(source_bytes: bytes) -> tuple[list[tuple[int, int, int, int]], bytes]:
    try:
        from PIL import Image
    except ImportError as error:
        raise SystemExit("Pillow is required to regenerate the loading backdrop") from error

    actual_hash = hashlib.sha256(source_bytes).hexdigest()
    if actual_hash != SOURCE_SHA256:
        raise ValueError(f"loading backdrop source asset hash mismatch: {actual_hash}")

    with Image.open(BytesIO(source_bytes)) as source:
        if source.size != (2_560, 1_440):
            raise ValueError(f"unexpected loading backdrop dimensions: {source.size}")
        resized = source.convert("RGB").resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)

    shaded_pixels: list[tuple[int, int, int]] = []
    for y in range(HEIGHT):
        alpha = shade_alpha(y / (HEIGHT - 1))
        source_row = resized.crop((0, y, WIDTH, y + 1)).tobytes()
        for offset in range(0, len(source_row), 3):
            red, green, blue = source_row[offset:offset + 3]
            shaded_pixels.append((
                int(red * (1.0 - alpha) + 10 * alpha + 0.5),
                int(green * (1.0 - alpha) + 25 * alpha + 0.5),
                int(blue * (1.0 - alpha) + 34 * alpha + 0.5),
            ))

    shaded = Image.new("RGB", (WIDTH, HEIGHT))
    shaded.putdata(shaded_pixels)
    indexed = shaded.quantize(
        colors=PALETTE_SIZE,
        method=Image.Quantize.MEDIANCUT,
        dither=Image.Dither.NONE,
    )
    palette_bytes = indexed.getpalette()
    if palette_bytes is None or len(palette_bytes) < PALETTE_SIZE * 3:
        raise ValueError("Pillow did not return a full 256-color palette")
    palette = [
        (
            palette_bytes[index * 3],
            palette_bytes[index * 3 + 1],
            palette_bytes[index * 3 + 2],
            255,
        )
        for index in range(PALETTE_SIZE)
    ]
    indices = indexed.tobytes()
    if len(indices) != WIDTH * HEIGHT:
        raise ValueError("Pillow returned an unexpected indexed image size")
    return palette, indices


def render_header(source_bytes: bytes) -> str:
    palette, indices = prepare_pixels(source_bytes)
    palette_lines = "\n".join(
        "    { " + ", ".join(f"{channel}U" for channel in color) + " },"
        for color in palette
    )
    encoded_indices = indices.hex()
    index_chunks = [
        encoded_indices[offset:offset + INDEX_CHUNK_LENGTH]
        for offset in range(0, len(encoded_indices), INDEX_CHUNK_LENGTH)
    ]
    index_lines = "\n".join(f'    "{chunk}",' for chunk in index_chunks)
    return f"""// Generated from assets/textures/loading_backdrop/seed42-relief.png.
// SHA-256 {SOURCE_SHA256}.
// Captured from the current prepared seed-42 terrain; see the adjacent provenance JSON.
// The {WIDTH}x{HEIGHT} downsample applies the existing loading-view shade stops before 256-color quantization.
#pragma once

#include "GenerationMapPresentation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace client
{{

inline constexpr uint32_t LOADING_BACKDROP_WIDTH = {WIDTH}U;
inline constexpr uint32_t LOADING_BACKDROP_HEIGHT = {HEIGHT}U;

inline constexpr std::array<GenerationMapRgba8, {PALETTE_SIZE}U> LOADING_BACKDROP_PALETTE = {{{{
{palette_lines}
}}}};

inline constexpr std::array<std::string_view, {len(index_chunks)}U> LOADING_BACKDROP_INDEX_HEX = {{{{
{index_lines}
}}}};

[[nodiscard]]
inline GenerationMapImageHandle loadingBackdropImage()
{{
    static GenerationMapImageHandle const image = [] {{
        auto expanded = std::make_shared<GenerationMapImage>();
        expanded->width = LOADING_BACKDROP_WIDTH;
        expanded->height = LOADING_BACKDROP_HEIGHT;
        expanded->rgba.reserve(static_cast<size_t>(LOADING_BACKDROP_WIDTH) * LOADING_BACKDROP_HEIGHT);

        auto hexValue = [](char const digit) -> uint8_t {{
            return static_cast<uint8_t>(digit >= '0' && digit <= '9'
                ? digit - '0'
                : digit - 'a' + 10);
        }};
        for (std::string_view const chunk : LOADING_BACKDROP_INDEX_HEX) {{
            for (size_t offset = 0U; offset < chunk.size(); offset += 2U) {{
                uint8_t const palette_index = static_cast<uint8_t>(
                    (hexValue(chunk[offset]) << 4U) | hexValue(chunk[offset + 1U])
                );
                expanded->rgba.push_back(LOADING_BACKDROP_PALETTE[palette_index]);
            }}
        }}
        return GenerationMapImageHandle{{ std::move(expanded) }};
    }}();
    return image;
}}

}} // namespace client
"""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the generated header is stale")
    args = parser.parse_args()

    source_bytes = SOURCE_PATH.read_bytes()
    generated = render_header(source_bytes)
    if args.check:
        if not OUTPUT_PATH.is_file() or OUTPUT_PATH.read_text(encoding="utf-8") != generated:
            print(f"{OUTPUT_PATH.relative_to(ROOT)} is stale; rerun {Path(__file__).name}", file=sys.stderr)
            return 1
        print(f"{OUTPUT_PATH.relative_to(ROOT)} is current")
        return 0

    if OUTPUT_PATH.is_file() and OUTPUT_PATH.read_text(encoding="utf-8") == generated:
        print(f"{OUTPUT_PATH.relative_to(ROOT)} is already current")
        return 0
    OUTPUT_PATH.write_text(generated, encoding="utf-8", newline="\n")
    print(f"wrote {OUTPUT_PATH.relative_to(ROOT)} ({OUTPUT_PATH.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

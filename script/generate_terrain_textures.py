#!/usr/bin/env python3
"""Extract pinned vanilla textures and generate the CPU terrain texture atlas."""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import zlib
from pathlib import Path
from zipfile import BadZipFile, ZipFile


ROOT = Path(__file__).resolve().parent.parent
ASSET_ROOT = ROOT / "assets/textures/terrain/vanilla-1.21.1"
HEADER_PATH = ROOT / "src/client/include/client/render/TerrainTextures.hpp"
CLIENT_SHA1 = "30c73b1c5da787909b2f73340419fdf13b9def88"
CLIENT_SHA256 = "499f6897d1837516680f3114072d8106e11c9adcd933fe5cf051b551089b0c99"
CLIENT_SIZE = 26_836_906
JAR_PREFIX = "assets/minecraft/textures/"

ASSET_SHA256 = {
    "block/stone.png": "33ac70ba8cb701087642a12c5fd29c1ba3006dc4730e9b014ed3a19ce7cdda7b",
    "block/grass_block_top.png": "52f44b64ce884a13dfe35e4b4da2e35898b790dd4d7a5ea536f1b0c0d56d29b4",
    "block/grass_block_side.png": "ab572ac7ebd373abdeb961de1f3cba0afcd91e1acb700745ece4f719790bae3d",
    "block/grass_block_side_overlay.png": "43d98318407d9d122b83468cf183d18722e548c2144f9c2ffe2021b5c3e770e0",
    "block/dirt.png": "1f0f0e619e7dfe683ea45b9254859f14b0ed5f1debdb242ea878828575be5bf0",
    "block/bedrock.png": "52f57443f329e53404c73c697ffd75770f1e2abf935a28315621fb0ca94b7571",
    "block/snow.png": "05201107fc2c60db4dc59f778a9ca7cb6101b2a332da696b33ce4b0cd2a02019",
    "block/ice.png": "4d158c24f2ec563d52806642ac7f4e20a5bda168af8c2ae528ed8e9ef1a58771",
    "colormap/grass.png": "d55308b1c707bc239b4be80ea81dad927827b9010d62f23f34297a99bc1ea4d6",
}

TILE_SIZE = 16
PIXELS_PER_TILE = TILE_SIZE * TILE_SIZE
FACE_NAMES = ("NegativeX", "PositiveX", "NegativeY", "PositiveY", "NegativeZ", "PositiveZ")
BLOCK_NAMES = ("Air", "Stone", "Grass", "Dirt", "Bedrock", "Snow", "Ice")
GRASS_TEMPERATURE = 0.8
GRASS_DOWNFALL = 0.4


def fail(message: str) -> None:
    raise ValueError(message)


def _paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def decode_png(data: bytes, label: str) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    signature = b"\x89PNG\r\n\x1a\n"
    if not data.startswith(signature):
        fail(f"{label}: invalid PNG signature")

    offset = len(signature)
    width = height = bit_depth = color_type = interlace = None
    palette: list[tuple[int, int, int]] = []
    palette_alpha = b""
    transparency = b""
    compressed = bytearray()
    saw_image_end = False

    while offset < len(data):
        if offset + 12 > len(data):
            fail(f"{label}: truncated PNG chunk header")
        chunk_length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4:offset + 8]
        chunk_start = offset + 8
        chunk_end = chunk_start + chunk_length
        if chunk_end + 4 > len(data):
            fail(f"{label}: truncated {chunk_type!r} chunk")
        chunk_data = data[chunk_start:chunk_end]
        expected_crc = struct.unpack_from(">I", data, chunk_end)[0]
        actual_crc = zlib.crc32(chunk_type + chunk_data) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            fail(f"{label}: bad CRC in {chunk_type.decode('ascii', 'replace')} chunk")

        if chunk_type == b"IHDR":
            if width is not None or chunk_length != 13:
                fail(f"{label}: invalid IHDR")
            width, height, bit_depth, color_type, compression, filtering, interlace = (
                struct.unpack(">IIBBBBB", chunk_data)
            )
            if compression != 0 or filtering != 0:
                fail(f"{label}: unsupported PNG compression/filter method")
        elif chunk_type == b"PLTE":
            if chunk_length % 3:
                fail(f"{label}: malformed palette")
            palette = [
                tuple(chunk_data[index:index + 3])
                for index in range(0, len(chunk_data), 3)
            ]
        elif chunk_type == b"tRNS":
            transparency = chunk_data
            if color_type == 3:
                palette_alpha = chunk_data
        elif chunk_type == b"IDAT":
            compressed.extend(chunk_data)
        elif chunk_type == b"IEND":
            if chunk_length != 0:
                fail(f"{label}: malformed IEND")
            saw_image_end = True
            offset = chunk_end + 4
            break

        offset = chunk_end + 4

    if not saw_image_end or offset != len(data):
        fail(f"{label}: missing IEND or trailing bytes")
    if width is None or height is None or bit_depth is None or color_type is None:
        fail(f"{label}: missing IHDR")
    if interlace != 0:
        fail(f"{label}: interlaced PNG is unsupported")

    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    channels = channels_by_type.get(color_type)
    if channels is None or bit_depth not in (2, 4, 8):
        fail(f"{label}: unsupported PNG color type or bit depth")
    if color_type in (2, 4, 6) and bit_depth != 8:
        fail(f"{label}: unsupported truecolor bit depth")
    if color_type == 3 and not palette:
        fail(f"{label}: indexed PNG has no palette")

    scanline_size = (width * channels * bit_depth + 7) // 8
    filter_bytes_per_pixel = max(1, (channels * bit_depth + 7) // 8)
    try:
        scanlines = zlib.decompress(compressed)
    except zlib.error as error:
        fail(f"{label}: invalid compressed image data ({error})")
    if len(scanlines) != height * (scanline_size + 1):
        fail(f"{label}: unexpected scanline byte count")

    raw_rows: list[bytearray] = []
    previous = bytearray(scanline_size)
    for row_index in range(height):
        start = row_index * (scanline_size + 1)
        filter_type = scanlines[start]
        source = scanlines[start + 1:start + 1 + scanline_size]
        row = bytearray(scanline_size)
        if filter_type > 4:
            fail(f"{label}: unsupported PNG filter {filter_type}")
        for index, source_byte in enumerate(source):
            left = row[index - filter_bytes_per_pixel] if index >= filter_bytes_per_pixel else 0
            above = previous[index]
            upper_left = (
                previous[index - filter_bytes_per_pixel]
                if index >= filter_bytes_per_pixel
                else 0
            )
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            else:
                predictor = _paeth(left, above, upper_left)
            row[index] = (source_byte + predictor) & 0xFF
        raw_rows.append(row)
        previous = row

    pixels: list[tuple[int, int, int, int]] = []
    for row in raw_rows:
        for x in range(width):
            raw_samples: list[int] = []
            for channel in range(channels):
                sample_index = x * channels + channel
                if bit_depth == 8:
                    raw_samples.append(row[sample_index])
                else:
                    bit_offset = sample_index * bit_depth
                    shift = 8 - bit_depth - bit_offset % 8
                    mask = (1 << bit_depth) - 1
                    raw_samples.append((row[bit_offset // 8] >> shift) & mask)

            scale = 255 // ((1 << bit_depth) - 1)
            if color_type == 0:
                gray = raw_samples[0] * scale
                transparent_gray = (
                    struct.unpack(">H", transparency)[0]
                    if len(transparency) == 2
                    else None
                )
                alpha = 0 if transparent_gray == raw_samples[0] else 255
                pixels.append((gray, gray, gray, alpha))
            elif color_type == 2:
                red, green, blue = raw_samples
                alpha = 255
                if len(transparency) == 6:
                    transparent_rgb = struct.unpack(">HHH", transparency)
                    if tuple(raw_samples) == transparent_rgb:
                        alpha = 0
                pixels.append((red, green, blue, alpha))
            elif color_type == 3:
                palette_index = raw_samples[0]
                if palette_index >= len(palette):
                    fail(f"{label}: palette index {palette_index} is out of range")
                red, green, blue = palette[palette_index]
                alpha = palette_alpha[palette_index] if palette_index < len(palette_alpha) else 255
                pixels.append((red, green, blue, alpha))
            elif color_type == 4:
                gray, alpha = raw_samples
                pixels.append((gray, gray, gray, alpha))
            else:
                pixels.append(tuple(raw_samples))

    return width, height, pixels


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def verify_asset_bytes(relative_path: str, data: bytes) -> None:
    expected = ASSET_SHA256[relative_path]
    actual = sha256(data)
    if actual != expected:
        fail(f"{relative_path}: SHA-256 mismatch: expected {expected}, got {actual}")


def extract_client_jar(client_jar: Path) -> None:
    client_bytes = client_jar.read_bytes()
    if len(client_bytes) != CLIENT_SIZE:
        fail(f"client JAR size mismatch: expected {CLIENT_SIZE}, got {len(client_bytes)}")
    sha1_digest = hashlib.sha1(client_bytes).hexdigest()
    if sha1_digest != CLIENT_SHA1:
        fail(f"client JAR SHA-1 mismatch: expected {CLIENT_SHA1}, got {sha1_digest}")
    sha256_digest = sha256(client_bytes)
    if sha256_digest != CLIENT_SHA256:
        fail(f"client JAR SHA-256 mismatch: expected {CLIENT_SHA256}, got {sha256_digest}")
    try:
        with ZipFile(client_jar) as archive:
            for relative_path in ASSET_SHA256:
                entry = JAR_PREFIX + relative_path
                data = archive.read(entry)
                verify_asset_bytes(relative_path, data)
                destination = ASSET_ROOT / relative_path
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(data)
    except (BadZipFile, KeyError) as error:
        fail(f"cannot extract official client asset: {error}")


def read_assets() -> dict[str, tuple[int, int, list[tuple[int, int, int, int]]]]:
    decoded = {}
    for relative_path, expected_sha256 in ASSET_SHA256.items():
        asset_path = ASSET_ROOT / relative_path
        if not asset_path.is_file():
            fail(f"missing asset {asset_path}; provide the official 1.21.1 client JAR")
        data = asset_path.read_bytes()
        if sha256(data) != expected_sha256:
            fail(f"{relative_path}: source PNG SHA-256 does not match the pinned asset")
        decoded[relative_path] = decode_png(data, relative_path)
    return decoded


def _image_pixels(
    images: dict[str, tuple[int, int, list[tuple[int, int, int, int]]]],
    relative_path: str,
) -> list[tuple[int, int, int, int]]:
    width, height, pixels = images[relative_path]
    expected = 256 if relative_path == "colormap/grass.png" else TILE_SIZE
    if width != expected or height != expected:
        fail(f"{relative_path}: expected {expected}x{expected}, got {width}x{height}")
    return pixels


def grass_tint(
    colormap: list[tuple[int, int, int, int]],
) -> tuple[int, int, int, int]:
    x = int((1.0 - GRASS_TEMPERATURE) * 255.0)
    humidity = GRASS_DOWNFALL * GRASS_TEMPERATURE
    y = int((1.0 - humidity) * 255.0)
    red, green, blue, _ = colormap[y * 256 + x]
    if (red, green, blue) != (0x91, 0xBD, 0x59):
        fail(f"unexpected vanilla Plains grass colormap sample at ({x},{y})")
    return red, green, blue, 255


def pack_rgba(pixel: tuple[int, int, int, int]) -> int:
    red, green, blue, alpha = pixel
    return red | (green << 8) | (blue << 16) | (alpha << 24)


def format_packed_rgba8(value: int) -> str:
    digits = f"{value:08X}"
    return f"0x{digits[:4]}'{digits[4:]}U"


def build_tiles(
    images: dict[str, tuple[int, int, list[tuple[int, int, int, int]]]],
) -> tuple[list[tuple[str, list[int]]], int]:
    textures = {
        key: _image_pixels(images, key)
        for key in ASSET_SHA256
    }
    face_sources = {
        "Air": (None, None, None, None, None, None),
        "Stone": ("block/stone.png",) * 6,
        "Grass": (
            "block/grass_block_side.png",
            "block/grass_block_side.png",
            "block/grass_block_side.png",
            "block/grass_block_side.png",
            "block/dirt.png",
            "block/grass_block_top.png",
        ),
        "Dirt": ("block/dirt.png",) * 6,
        "Bedrock": ("block/bedrock.png",) * 6,
        "Snow": ("block/snow.png",) * 6,
        "Ice": ("block/ice.png",) * 6,
    }
    tiles: list[tuple[str, list[int]]] = []
    for block_name in BLOCK_NAMES:
        for face_name, source_path in zip(FACE_NAMES, face_sources[block_name]):
            source_pixels = (
                [(0, 0, 0, 0)] * PIXELS_PER_TILE
                if source_path is None
                else textures[source_path]
            )
            tiles.append(
                (f"{block_name} / {face_name}", [pack_rgba(pixel) for pixel in source_pixels])
            )

    overlay_pixels = textures["block/grass_block_side_overlay.png"]
    tiles.append(
        (
            "grass_block_side_overlay",
            [pack_rgba(pixel) for pixel in overlay_pixels],
        )
    )
    return tiles, pack_rgba(grass_tint(textures["colormap/grass.png"]))


def generate_header(
    tiles: list[tuple[str, list[int]]],
    grass_tint_rgba8: int,
) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace client::render::terrain_textures {",
        "",
        "inline constexpr uint32_t TILE_SIZE = 16U;",
        "inline constexpr uint32_t PIXELS_PER_TILE = TILE_SIZE * TILE_SIZE;",
        "inline constexpr uint32_t FACE_COUNT = 6U;",
        "inline constexpr uint32_t MATERIAL_COUNT = 7U;",
        "inline constexpr uint32_t TILE_COUNT = MATERIAL_COUNT * FACE_COUNT + 1U;",
        "inline constexpr uint32_t GRASS_SIDE_OVERLAY_TILE_INDEX = MATERIAL_COUNT * FACE_COUNT;",
        "inline constexpr uint32_t INVALID_TILE_INDEX = 0xffff'ffffU;",
        f"inline constexpr uint32_t GRASS_TINT_RGBA8 = {format_packed_rgba8(grass_tint_rgba8)};",
        "",
        "[[nodiscard]]",
        "constexpr uint32_t tileIndex(uint32_t const block_id, uint32_t const face_direction) noexcept",
        "{",
        "    if (block_id >= MATERIAL_COUNT || face_direction >= FACE_COUNT) {",
        "        return INVALID_TILE_INDEX;",
        "    }",
        "    return block_id * FACE_COUNT + face_direction;",
        "}",
        "",
        "// Each pixel packs R in bits 0-7, G in 8-15, B in 16-23, and A in 24-31.",
        "inline constexpr std::array<uint32_t, static_cast<uint64_t>(TILE_COUNT) * PIXELS_PER_TILE> RGBA8{",
    ]
    for tile_index, (description, pixels) in enumerate(tiles):
        lines.append(f"    // Tile {tile_index}: {description}.")
        for start in range(0, len(pixels), 8):
            entries = ", ".join(format_packed_rgba8(pixel) for pixel in pixels[start:start + 8])
            is_last = tile_index == len(tiles) - 1 and start + 8 >= len(pixels)
            suffix = "" if is_last else ","
            lines.append(f"    {entries}{suffix}")
    lines.extend(
        [
            "};",
            "",
            "static_assert(TILE_COUNT == 43U);",
            "static_assert(tileIndex(2U, 5U) == 17U);",
            "static_assert(GRASS_SIDE_OVERLAY_TILE_INDEX == 42U);",
            "",
            "} // namespace client::render::terrain_textures",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--client-jar",
        type=Path,
        help="extract assets from the pinned official Minecraft Java 1.21.1 client JAR",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify source PNGs and require the checked-in header to match generation",
    )
    args = parser.parse_args()
    if args.check and args.client_jar:
        parser.error("--check cannot be combined with --client-jar")

    if args.client_jar:
        extract_client_jar(args.client_jar)

    generated = generate_header(*build_tiles(read_assets()))
    if args.check:
        if not HEADER_PATH.is_file() or HEADER_PATH.read_text(encoding="utf-8") != generated:
            print(f"generated header differs: {HEADER_PATH}", file=sys.stderr)
            return 1
        print("official source PNG hashes and generated header match")
        return 0

    HEADER_PATH.parent.mkdir(parents=True, exist_ok=True)
    HEADER_PATH.write_text(generated, encoding="utf-8", newline="\n")
    print(f"wrote {HEADER_PATH}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"terrain texture generation failed: {error}", file=sys.stderr)
        raise SystemExit(1)

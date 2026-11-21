"""Minimal PNG / JPEG helpers (standard library only): image size, PNG encode / decode, crop.

Used by the HTML adapter (image sizes, cropping the preview for baked elements) and by the conformance case
generator (small test textures). Decoding handles 8-bit greyscale / RGB / RGBA / palette PNGs (what browsers write).
"""

from __future__ import annotations

import struct
import zlib

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def image_size(data: bytes) -> tuple[int, int] | None:
    """(width, height) of a PNG, JPEG, GIF or WebP (VP8 / VP8L / VP8X) image, or None."""
    if data[:8] == PNG_SIGNATURE and len(data) >= 24:
        return struct.unpack(">II", data[16:24])
    if data[:6] in (b"GIF87a", b"GIF89a") and len(data) >= 10:
        return struct.unpack("<HH", data[6:10])
    if data[:2] == b"\xff\xd8":
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                i += 1
                continue
            marker = data[i + 1]
            if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
                i += 2
                continue
            length = struct.unpack(">H", data[i + 2:i + 4])[0]
            if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
                h, w = struct.unpack(">HH", data[i + 5:i + 9])
                return w, h
            i += 2 + length
        return None
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        chunk = data[12:16]
        if chunk == b"VP8X":
            w = 1 + int.from_bytes(data[24:27], "little")
            h = 1 + int.from_bytes(data[27:30], "little")
            return w, h
        if chunk == b"VP8L":
            b = int.from_bytes(data[21:25], "little")
            return 1 + (b & 0x3FFF), 1 + ((b >> 14) & 0x3FFF)
        if chunk == b"VP8 ":
            w, h = struct.unpack("<HH", data[26:30])
            return w & 0x3FFF, h & 0x3FFF
    return None


def _chunk(kind: bytes, payload: bytes) -> bytes:
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)


def encode_png(width: int, height: int, rgba: bytes | bytearray) -> bytes:
    """8-bit RGBA PNG from ``width * height * 4`` bytes (rows top to bottom)."""
    if len(rgba) != width * height * 4:
        raise ValueError("pixel buffer size does not match width x height x 4")
    stride = width * 4
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return PNG_SIGNATURE + _chunk(b"IHDR", header) + _chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + _chunk(b"IEND", b"")


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def decode_png(data: bytes) -> tuple[int, int, bytearray]:
    """(width, height, RGBA bytes). 8-bit, non-interlaced PNGs only (raises ValueError otherwise)."""
    if data[:8] != PNG_SIGNATURE:
        raise ValueError("not a PNG")
    pos = 8
    idat = bytearray()
    palette = b""
    trns = b""
    width = height = depth = color_type = interlace = 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", payload)
        elif kind == b"PLTE":
            palette = payload
        elif kind == b"tRNS":
            trns = payload
        elif kind == b"IDAT":
            idat += payload
        elif kind == b"IEND":
            break
    if depth != 8 or interlace:
        raise ValueError(f"unsupported PNG (bit depth {depth}, interlace {interlace})")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"unsupported PNG colour type {color_type}")
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    i = 0
    for y in range(height):
        ftype = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if ftype == 1:
            for x in range(channels, stride):
                line[x] = (line[x] + line[x - channels]) & 0xFF
        elif ftype == 2:
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif ftype == 3:
            for x in range(stride):
                left = line[x - channels] if x >= channels else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 0xFF
        elif ftype == 4:
            for x in range(stride):
                left = line[x - channels] if x >= channels else 0
                up_left = prev[x - channels] if x >= channels else 0
                line[x] = (line[x] + _paeth(left, prev[x], up_left)) & 0xFF
        o = y * width * 4
        for x in range(width):
            if channels == 4:
                out[o:o + 4] = line[x * 4:x * 4 + 4]
            elif channels == 3:
                out[o:o + 3] = line[x * 3:x * 3 + 3]
                out[o + 3] = 255
            elif channels == 2:
                g = line[x * 2]
                out[o:o + 4] = bytes((g, g, g, line[x * 2 + 1]))
            elif color_type == 3:
                idx = line[x]
                out[o:o + 3] = palette[idx * 3:idx * 3 + 3]
                out[o + 3] = trns[idx] if idx < len(trns) else 255
            else:
                g = line[x]
                out[o:o + 4] = bytes((g, g, g, 255))
            o += 4
        prev = line
    return width, height, out


def crop_rgba(width: int, height: int, rgba: bytes | bytearray, x: int, y: int, w: int, h: int) -> bytearray:
    """Crop (clamped to the image; pixels outside are transparent)."""
    out = bytearray(w * h * 4)
    for row in range(h):
        sy = y + row
        if sy < 0 or sy >= height:
            continue
        x0, x1 = max(x, 0), min(x + w, width)
        if x1 <= x0:
            continue
        src = (sy * width + x0) * 4
        dst = (row * w + (x0 - x)) * 4
        out[dst:dst + (x1 - x0) * 4] = rgba[src:src + (x1 - x0) * 4]
    return out

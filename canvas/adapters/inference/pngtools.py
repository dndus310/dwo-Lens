"""Standard-library image helpers for the inference adapter.

- :func:`sniff` tells PNG from JPEG by magic bytes.
- :func:`png_size` / :func:`jpeg_size` read the pixel size from the header.
- :func:`decode_png` decodes a non-interlaced 8-bit PNG (grey, grey+alpha, RGB, RGBA, palette) into RGBA rows.
- :func:`dominant_colors` samples the decoded image for colour hints given to the model.
- :func:`crop` / :func:`encode_png` cut a region out (icon crops used as baked assets).

JPEG pixels are not decoded (no codec in the standard library): JPEG input still works, but colour hints and icon
crops are skipped.
"""

from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
JPEG_MAGIC = b"\xff\xd8\xff"


class PngError(ValueError):
    pass


def sniff(data: bytes) -> str | None:
    """``"image/png"``, ``"image/jpeg"`` or None."""
    if data.startswith(PNG_MAGIC):
        return "image/png"
    if data.startswith(JPEG_MAGIC):
        return "image/jpeg"
    return None


def png_size(data: bytes) -> tuple[int, int]:
    if not data.startswith(PNG_MAGIC) or data[12:16] != b"IHDR":
        raise PngError("not a PNG file")
    return struct.unpack(">II", data[16:24])


def jpeg_size(data: bytes) -> tuple[int, int] | None:
    """Width and height from the first SOFn marker, or None."""
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
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            h, w = struct.unpack(">HH", data[i + 5:i + 9])
            return w, h
        i += 2 + length
    return None


@dataclass
class Image:
    width: int
    height: int
    rows: list[bytearray]  # RGBA, 4 bytes per pixel

    def pixel(self, x: int, y: int) -> tuple[int, int, int, int]:
        row = self.rows[y]
        o = x * 4
        return row[o], row[o + 1], row[o + 2], row[o + 3]


def _chunks(data: bytes):
    pos = len(PNG_MAGIC)
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        yield kind, body
        pos += 12 + length
        if kind == b"IEND":
            break


def _unfilter(raw: bytes, width: int, height: int, bpp: int) -> list[bytearray]:
    stride = width * bpp
    rows: list[bytearray] = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(height):
        ftype = raw[pos]
        line = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ftype == 2:  # Up
            line = bytearray(((a + b) & 0xFF) for a, b in zip(line, prev))
        elif ftype == 3:  # Average
            for i in range(stride):
                left = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pred) & 0xFF
        else:
            raise PngError(f"bad filter type {ftype}")
        rows.append(line)
        prev = line
    return rows


def decode_png(data: bytes) -> Image:
    """Decode to RGBA. Supports bit depth 8, colour types 0/2/3/4/6, no interlace."""
    width, height = png_size(data)
    bit_depth, color_type, _, _, interlace = struct.unpack(">BBBBB", data[24:29])
    if bit_depth != 8:
        raise PngError(f"bit depth {bit_depth} not supported (8 only)")
    if interlace:
        raise PngError("interlaced PNG not supported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise PngError(f"colour type {color_type} not supported")
    idat = bytearray()
    palette = b""
    trns = b""
    for kind, body in _chunks(data):
        if kind == b"IDAT":
            idat += body
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            trns = body
    rows = _unfilter(zlib.decompress(bytes(idat)), width, height, channels)
    if color_type == 6:
        return Image(width, height, rows)
    out: list[bytearray] = []
    for line in rows:
        rgba = bytearray(width * 4)
        if color_type == 2:
            rgba[0::4], rgba[1::4], rgba[2::4] = line[0::3], line[1::3], line[2::3]
            rgba[3::4] = b"\xff" * width
        elif color_type == 0:
            rgba[0::4] = rgba[1::4] = rgba[2::4] = line
            rgba[3::4] = b"\xff" * width
        elif color_type == 4:
            rgba[0::4] = rgba[1::4] = rgba[2::4] = line[0::2]
            rgba[3::4] = line[1::2]
        else:  # palette
            for x, index in enumerate(line):
                o = x * 4
                rgba[o:o + 3] = palette[index * 3:index * 3 + 3]
                rgba[o + 3] = trns[index] if index < len(trns) else 255
        out.append(rgba)
    return Image(width, height, out)


def encode_png(image: Image) -> bytes:
    """RGBA 8-bit PNG, filter 0."""
    raw = b"".join(b"\x00" + bytes(row) for row in image.rows)

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", image.width, image.height, 8, 6, 0, 0, 0)
    return PNG_MAGIC + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def crop(image: Image, x: float, y: float, w: float, h: float) -> Image | None:
    """Region clamped to the image (rounded outward), or None if empty."""
    x0 = max(0, int(x))
    y0 = max(0, int(y))
    x1 = min(image.width, int(-(-(x + w) // 1)))
    y1 = min(image.height, int(-(-(y + h) // 1)))
    if x1 <= x0 or y1 <= y0:
        return None
    return Image(x1 - x0, y1 - y0, [bytearray(image.rows[r][x0 * 4:x1 * 4]) for r in range(y0, y1)])


def dominant_colors(image: Image, count: int = 8, samples: int = 40000, merge: float = 16.0) -> list[dict]:
    """Most frequent colours (opaque pixels, 5-bit buckets averaged back to exact colours, near ones merged).

    Returns ``[{"hex": "#rrggbb", "rgb": [r, g, b] (0..1), "share": 0..1}]`` sorted by share.
    """
    total = image.width * image.height
    step = max(1, int((total / max(1, samples)) ** 0.5))
    buckets: dict[int, list[int]] = {}
    seen = 0
    for y in range(0, image.height, step):
        row = image.rows[y]
        for x in range(0, image.width, step):
            o = x * 4
            if row[o + 3] < 128:
                continue
            r, g, b = row[o], row[o + 1], row[o + 2]
            key = (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
            acc = buckets.get(key)
            if acc is None:
                buckets[key] = [r, g, b, 1]
            else:
                acc[0] += r
                acc[1] += g
                acc[2] += b
                acc[3] += 1
            seen += 1
    if not seen:
        return []
    # Merge near-identical buckets (anti-aliasing, gradients) into the most frequent one: distance < merge (0..255)
    merged: list[list[float]] = []  # [r, g, b, n]
    for r, g, b, n in sorted(buckets.values(), key=lambda a: -a[3]):
        rgb = (r / n, g / n, b / n)
        for m in merged:
            if sum((m[i] - rgb[i]) ** 2 for i in range(3)) < merge ** 2:
                m[3] += n
                break
        else:
            merged.append([rgb[0], rgb[1], rgb[2], n])
    out = []
    for r, g, b, n in sorted(merged, key=lambda a: -a[3])[:count]:
        rgb = (round(r), round(g), round(b))
        out.append({"hex": "#%02x%02x%02x" % rgb, "rgb": [round(c / 255, 3) for c in rgb], "share": round(n / seen, 3)})
    return out

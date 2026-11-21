"""DwoCanvas HTML/CSS adapter: an HTML page -> a dwocanvas/2 document.

    python canvas/adapters/html/html2dwocanvas.py page.html -o page.dwocanvas.json [--width 1280 --height 720] [--name Screen]

A headless Chromium browser (Edge or Chrome) lays the page out at the requested viewport. ``extractor.js`` walks the
DOM with getComputedStyle / getBoundingClientRect and returns a raw tree; this module maps it to the schema
(flex -> horizontal / vertical / wrap, grid -> grid, paints, strokes, radii, shadows, text), embeds images and the
1x screenshot (document ``preview``), fills role / variable / widgetName (data-* attributes, then the naming
rules) and validates the result with the shared validator before writing it.

Standard library only. See README.md next to this file.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
CANVAS_PYTHON = HERE.parents[1] / "python"
if str(CANVAS_PYTHON) not in sys.path:
    sys.path.insert(0, str(CANVAS_PYTHON))
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from dwocanvas import naming  # noqa: E402
from dwocanvas import validate as validate_module  # noqa: E402

import pngio  # noqa: E402

ADAPTER = "dwocanvas-html/0.1.0"
EXTRACTOR = HERE / "extractor.js"
CAPABILITIES = ["layout.flow", "paint.gradient", "paint.image", "effects", "preview"]
DEFAULT_WIDTH, DEFAULT_HEIGHT = 1280, 720

BROWSER_CANDIDATES = [
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
    "/usr/bin/microsoft-edge", "/usr/bin/google-chrome", "/usr/bin/chromium", "/usr/bin/chromium-browser",
    "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge",
    "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
]


class AdapterError(Exception):
    """The page could not be extracted (no browser, browser failure, unreadable input)."""


# =====================================================================================================================
# Browser

def find_browsers(explicit: str | None = None) -> list[str]:
    """Browser executables to try, in order: ``explicit``, $DWOCANVAS_BROWSER, Edge, Chrome."""
    out: list[str] = []
    for candidate in [explicit, os.environ.get("DWOCANVAS_BROWSER")] + BROWSER_CANDIDATES:
        if candidate and os.path.isfile(candidate) and candidate not in out:
            out.append(candidate)
    return out


def file_url(path: str | os.PathLike) -> str:
    return Path(path).resolve().as_uri()


WRAPPER = """<!doctype html>
<html><head><meta charset="utf-8"><style>
html, body {{ margin: 0; padding: 0; overflow: hidden; background: #fff; }}
iframe {{ position: absolute; left: 0; top: 0; width: {w}px; height: {h}px; border: 0; display: block; }}
</style></head><body><iframe src="{src}"></iframe>{script}</body></html>
"""

# The extractor posts its JSON to the wrapper (base64 so it survives --dump-dom serialisation)
RECEIVER = """<script>
window.addEventListener('message', function (e) {
  if (!e.data || typeof e.data.dwocanvas !== 'string') { return; }
  var pre = document.createElement('pre');
  pre.id = 'dwocanvas-output';
  pre.textContent = btoa(unescape(encodeURIComponent(e.data.dwocanvas)));
  document.body.appendChild(pre);
});
</script>"""


def _browser_args(browser: str, profile: str, width: int, height: int, budget_ms: int) -> list[str]:
    return [
        browser, "--headless=new", "--disable-gpu", "--force-device-scale-factor=1", f"--window-size={width},{height}",
        "--hide-scrollbars", "--allow-file-access-from-files", "--no-first-run", "--no-default-browser-check",
        # Greyscale text anti-aliasing, like Slate (sub-pixel LCD text adds colour fringes UE never draws)
        "--disable-lcd-text",
        "--disable-extensions", "--disable-sync", "--mute-audio", f"--user-data-dir={profile}",
        f"--virtual-time-budget={budget_ms}", "--run-all-compositor-stages-before-draw",
    ]


def inject(html: str, base_href: str, script: str) -> str:
    """Page copy for extraction: utf-8 meta + <base> first in <head>, the extractor script at the end of <body>."""
    head = f'<meta charset="utf-8"><base href="{base_href}">'
    m = re.search(r"<head\b[^>]*>", html, re.I)
    if m:
        html = html[:m.end()] + head + html[m.end():]
    else:
        m = re.search(r"<html\b[^>]*>", html, re.I)
        html = (html[:m.end()] + "<head>" + head + "</head>" + html[m.end():]) if m else ("<head>" + head + "</head>" + html)
    tag = "<script>\n" + script + "\n</script>"
    m = None
    for m in re.finditer(r"</body\s*>", html, re.I):
        pass
    return (html[:m.start()] + tag + html[m.start():]) if m else (html + tag)


def read_html(path: Path) -> str:
    data = path.read_bytes()
    m = re.search(rb'<meta[^>]+charset=["\']?([\w-]+)', data[:4096], re.I)
    encoding = m.group(1).decode("ascii", "replace") if m else "utf-8"
    try:
        return data.decode(encoding)
    except (LookupError, UnicodeDecodeError):
        return data.decode("utf-8", "replace")


def page_meta(html: str) -> dict[str, str]:
    """``<meta name="dwocanvas:viewport" content="400x300">`` / ``dwocanvas:name`` defaults for the CLI."""
    out = {}
    for m in re.finditer(r"<meta\s+[^>]*>", html, re.I):
        tag = m.group(0)
        name = re.search(r'name=["\']dwocanvas:([\w-]+)["\']', tag, re.I)
        content = re.search(r'content=["\']([^"\']*)["\']', tag, re.I)
        if name and content:
            out[name.group(1).lower()] = content.group(1)
    return out


def extract(html_path: str | os.PathLike, width: int, height: int, browser: str | None = None, preview: bool = True,
            timeout: float = 90.0, budget_ms: int = 5000, keep_temp: bool = False) -> tuple[dict, bytes | None, str]:
    """Lay the page out in a headless browser. Returns ``(raw_tree, preview_png_or_None, browser_used)``."""
    html_path = Path(html_path).resolve()
    if not html_path.is_file():
        raise AdapterError(f"{html_path}: no such file")
    browsers = find_browsers(browser)
    if not browsers:
        raise AdapterError("No Chromium browser found (Edge / Chrome). Pass --browser or set DWOCANVAS_BROWSER.")

    html = read_html(html_path)
    script = EXTRACTOR.read_text(encoding="utf-8")
    temp = Path(tempfile.mkdtemp(prefix="dwocanvas-html-"))
    errors: list[str] = []
    try:
        copy = temp / "page.html"
        copy.write_text(inject(html, html_path.parent.as_uri() + "/", script), encoding="utf-8")
        (temp / "extract.html").write_text(WRAPPER.format(w=width, h=height, src=file_url(copy), script=RECEIVER), encoding="utf-8")
        (temp / "shot.html").write_text(WRAPPER.format(w=width, h=height, src=file_url(html_path), script=""), encoding="utf-8")

        for exe in browsers:
            profile = temp / ("profile-" + Path(exe).stem)
            args = _browser_args(exe, str(profile), width, height, budget_ms)
            try:
                result = subprocess.run(args + ["--dump-dom", file_url(temp / "extract.html")], capture_output=True,
                                        timeout=timeout)
            except (OSError, subprocess.TimeoutExpired) as e:
                errors.append(f"{exe}: {e}")
                continue
            out = result.stdout.decode("utf-8", "replace")
            m = re.search(r'<pre id="dwocanvas-output">([A-Za-z0-9+/=\s]*)</pre>', out)
            if not m:
                errors.append(f"{exe}: no extractor output (exit code {result.returncode})")
                continue
            raw = json.loads(base64.b64decode(m.group(1)).decode("utf-8"))
            if "error" in raw:
                raise AdapterError(f"Extractor failed in {Path(exe).name}: {raw['error']}")

            png = None
            if preview:
                shot = temp / "preview.png"
                try:
                    subprocess.run(args + [f"--screenshot={shot}", file_url(temp / "shot.html")], capture_output=True, timeout=timeout)
                except (OSError, subprocess.TimeoutExpired) as e:
                    errors.append(f"{exe}: screenshot failed: {e}")
                if shot.is_file():
                    png = normalize_preview(shot.read_bytes(), width, height)
            return raw, png, exe
        raise AdapterError("Browser extraction failed:\n  " + "\n  ".join(errors))
    finally:
        if keep_temp:
            print(f"temp files kept in {temp}", file=sys.stderr)
        else:
            shutil.rmtree(temp, ignore_errors=True)


def normalize_preview(png: bytes, width: int, height: int) -> bytes:
    """Crop the screenshot to the viewport (some browser builds add a frame) and re-encode it as 8-bit RGBA."""
    size = pngio.image_size(png)
    if size == (width, height):
        return png
    w, h, rgba = pngio.decode_png(png)
    return pngio.encode_png(width, height, pngio.crop_rgba(w, h, rgba, 0, 0, width, height))


# =====================================================================================================================
# CSS value parsing

NAMED_COLORS = {
    "black": (0, 0, 0), "white": (255, 255, 255), "red": (255, 0, 0), "green": (0, 128, 0), "blue": (0, 0, 255),
    "gray": (128, 128, 128), "grey": (128, 128, 128), "silver": (192, 192, 192), "yellow": (255, 255, 0),
    "orange": (255, 165, 0), "purple": (128, 0, 128), "transparent": None,
}


def split_top(text: str, sep: str = ",") -> list[str]:
    """Split on ``sep`` outside parentheses / quotes. ``sep=" "`` splits on whitespace runs."""
    parts, depth, quote, cur = [], 0, "", []
    for ch in text:
        if quote:
            cur.append(ch)
            if ch == quote:
                quote = ""
            continue
        if ch in "\"'":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if depth == 0 and ((sep == " " and ch.isspace()) or ch == sep):
            parts.append("".join(cur))
            cur = []
            continue
        cur.append(ch)
    parts.append("".join(cur))
    return [p.strip() for p in parts if p.strip()]


def px(value: Any, default: float | None = 0.0) -> float | None:
    """'12px' / '12' -> 12.0; 'normal' / 'auto' / '' -> default."""
    if value is None:
        return default
    m = re.fullmatch(r"\s*(-?[\d.]+(?:e-?\d+)?)(px)?\s*", str(value))
    return float(m.group(1)) if m else default


def r4(v: float) -> float:
    v = round(float(v), 4)
    return 0.0 if v == 0 else v


def r2(v: float) -> float:
    v = round(float(v), 2)
    return 0.0 if v == 0 else v


def _channel(text: str, scale: float) -> float:
    text = text.strip()
    if text.endswith("%"):
        return float(text[:-1]) / 100.0
    if text == "none":
        return 0.0
    return float(text) / scale


def parse_color(text: str | None) -> list[float] | None:
    """CSS colour -> sRGB 0..1 [r, g, b, a] (None for 'transparent' / unparseable / 'none')."""
    if not text:
        return None
    text = text.strip().lower()
    m = re.fullmatch(r"rgba?\((.*)\)", text)
    if m:
        body = m.group(1)
        alpha = 1.0
        if "/" in body:
            body, a = body.split("/", 1)
            alpha = _channel(a, 1.0)
        parts = [p for p in re.split(r"[,\s]+", body.strip()) if p]
        if len(parts) == 4:
            alpha = _channel(parts[3], 1.0)
        if len(parts) < 3:
            return None
        rgb = [_channel(p, 255.0) for p in parts[:3]]
        return [r4(min(max(c, 0.0), 1.0)) for c in rgb] + [r4(min(max(alpha, 0.0), 1.0))]
    m = re.fullmatch(r"color\(srgb\s+([^)/]+)(?:/\s*([^)]+))?\)", text)
    if m:
        rgb = [float(p) for p in m.group(1).split()][:3]
        alpha = _channel(m.group(2), 1.0) if m.group(2) else 1.0
        return [r4(c) for c in rgb] + [r4(alpha)]
    m = re.fullmatch(r"#([0-9a-f]{3,8})", text)
    if m:
        h = m.group(1)
        if len(h) in (3, 4):
            h = "".join(c * 2 for c in h)
        if len(h) not in (6, 8):
            return None
        vals = [int(h[i:i + 2], 16) / 255.0 for i in range(0, len(h), 2)]
        return [r4(v) for v in vals] + ([] if len(vals) == 4 else [1.0])
    if text in NAMED_COLORS:
        rgb = NAMED_COLORS[text]
        return None if rgb is None else [r4(c / 255.0) for c in rgb] + [1.0]
    return None


def color_hex(color: list[float]) -> str:
    vals = [round(c * 255) for c in color[:3]] + ([round(color[3] * 255)] if len(color) > 3 and color[3] < 1 else [])
    return "#" + "".join(f"{v:02x}" for v in vals)


def same_color(a: list[float] | None, b: list[float] | None) -> bool:
    return bool(a and b) and all(abs(x - y) <= 1.5 / 255 for x, y in zip(a + [1.0] * (4 - len(a)), b + [1.0] * (4 - len(b))))


def parse_angle(text: str) -> float | None:
    """CSS angle -> degrees."""
    m = re.fullmatch(r"(-?[\d.]+)(deg|grad|rad|turn)", text.strip())
    if not m:
        return None
    v = float(m.group(1))
    return {"deg": v, "grad": v * 0.9, "rad": math.degrees(v), "turn": v * 360.0}[m.group(2)]


COLOR_START = re.compile(r"^(rgba?\(|hsla?\(|color\(|#|transparent\b|currentcolor\b|[a-z]+\s*$|[a-z]+\s+-?[\d.])", re.I)


def is_color_token(text: str) -> bool:
    text = text.strip()
    if re.match(r"^(to\b|at\b|from\b|circle\b|ellipse\b|closest|farthest|-?[\d.]+(deg|grad|rad|turn|px|%))", text, re.I):
        return False
    return bool(COLOR_START.match(text))


def parse_stops(args: list[str], length: float, unit_full: float = 100.0) -> tuple[list[dict], bool]:
    """Colour stops -> [{position 0..1, color}] (missing positions spread evenly). Returns (stops, had_hints)."""
    raw: list[tuple[list[float], float | None]] = []
    hints = False
    for arg in args:
        m = re.match(r"^((?:rgba?|hsla?|color)\([^)]*\)|#[0-9a-fA-F]+|[a-zA-Z]+)\s*(.*)$", arg.strip())
        if not m:
            continue
        color = parse_color(m.group(1))
        rest = m.group(2).strip()
        if color is None:
            if parse_position(m.group(1), length) is not None or re.match(r"^-?[\d.]", arg.strip()):
                hints = True  # colour hint (bare position)
                continue
            color = [0.0, 0.0, 0.0, 0.0]
        positions = [parse_position(p, length, unit_full) for p in rest.split()] if rest else []
        if not positions:
            raw.append((color, None))
        for pos in positions:
            raw.append((color, pos))
    if not raw:
        return [], hints
    pos = [p for _, p in raw]
    if pos[0] is None:
        pos[0] = 0.0
    if pos[-1] is None:
        pos[-1] = 1.0
    # monotonic + interpolate gaps
    for i in range(1, len(pos)):
        if pos[i] is not None and pos[i] < max(p for p in pos[:i] if p is not None):
            pos[i] = max(p for p in pos[:i] if p is not None)
    i = 0
    while i < len(pos):
        if pos[i] is None:
            j = i
            while pos[j] is None:
                j += 1
            start, end = pos[i - 1], pos[j]
            for k in range(i, j):
                pos[k] = start + (end - start) * (k - i + 1) / (j - i + 1)
            i = j
        i += 1
    return [{"position": r4(p), "color": c} for (c, _), p in zip(raw, pos)], hints


def parse_position(text: str, length: float, unit_full: float = 100.0) -> float | None:
    text = text.strip()
    if text.endswith("%"):
        try:
            return float(text[:-1]) / unit_full
        except ValueError:
            return None
    value = px(text, None)
    if value is not None and length > 0:
        return value / length
    angle = parse_angle(text)
    if angle is not None:
        return angle / 360.0
    return None


def parse_bg_position(text: str, w: float, h: float) -> tuple[float, float]:
    """'at' position of a radial / conic gradient -> px in the box."""
    parts = text.split()
    keywords = {"left": ("x", 0.0), "right": ("x", 1.0), "top": ("y", 0.0), "bottom": ("y", 1.0)}
    x = y = None
    rest = []
    for p in parts:
        if p in keywords:
            axis, frac = keywords[p]
            if axis == "x":
                x = frac * w
            else:
                y = frac * h
        elif p == "center":
            rest.append(None)
        else:
            rest.append(p)

    def value(p, size):
        if p is None:
            return size / 2
        if p.endswith("%"):
            return float(p[:-1]) / 100 * size
        return px(p, size / 2)

    if x is None and rest:
        x = value(rest.pop(0), w)
    if y is None and rest:
        y = value(rest.pop(0), h)
    return (w / 2 if x is None else x), (h / 2 if y is None else y)


def parse_shadows(text: str | None) -> list[dict]:
    """box-shadow / text-shadow (computed) -> [{inset, color, offset, blur, spread}]."""
    if not text or text == "none":
        return []
    out = []
    for item in split_top(text):
        inset = False
        color = None
        lengths = []
        for token in split_top(item, " "):
            if token == "inset":
                inset = True
            elif px(token, None) is not None:
                lengths.append(px(token))
            else:
                color = parse_color(token) or color
        while len(lengths) < 4:
            lengths.append(0.0)
        out.append({"inset": inset, "color": color or [0.0, 0.0, 0.0, 1.0], "offset": [r2(lengths[0]), r2(lengths[1])],
                    "blur": r2(lengths[2]), "spread": r2(lengths[3])})
    return out


def parse_filter_blur(text: str | None) -> float | None:
    m = re.search(r"blur\(\s*([\d.]+)px\s*\)", text or "")
    return float(m.group(1)) if m else None


WEIGHT_NAMES = {100: "Thin", 200: "ExtraLight", 300: "Light", 400: "Regular", 500: "Medium", 600: "SemiBold", 700: "Bold",
                800: "ExtraBold", 900: "Black"}


def style_name(weight: int, italic: bool) -> str:
    """Conventional style name for a CSS weight ('Bold Italic'). Informative in the schema, but UE's typeface lookup
    falls back to it, so it is always written."""
    name = WEIGHT_NAMES[min(WEIGHT_NAMES, key=lambda w: abs(w - weight))]
    if italic:
        return "Italic" if name == "Regular" else f"{name} Italic"
    return name


def family_name(font_family: str) -> str:
    first = split_top(font_family or "")[0] if font_family else "Roboto"
    return first.strip().strip("\"'")


# =====================================================================================================================
# Gradients

def linear_gradient(args: list[str], w: float, h: float) -> tuple[dict | None, list[str]]:
    notes = []
    direction = 180.0  # to bottom
    if args and not is_color_token(args[0]):
        config = args.pop(0).strip().lower()
        angle = parse_angle(config)
        if angle is not None:
            direction = angle
        elif config.startswith("to "):
            words = config[3:].split()
            dx = (1 if "right" in words else -1 if "left" in words else 0)
            dy = (1 if "bottom" in words else -1 if "top" in words else 0)
            if dx and dy:
                # Corner: the 50% line runs through the other two corners
                direction = math.degrees(math.atan2(dx * h, -dy * w))
            else:
                direction = {(0, -1): 0.0, (1, 0): 90.0, (0, 1): 180.0, (-1, 0): 270.0}.get((dx, dy), 180.0)
        else:
            notes.append(f"Unsupported linear-gradient direction '{config}': using 'to bottom'")
    rad = math.radians(direction)
    dx, dy = math.sin(rad), -math.cos(rad)
    length = abs(w * dx) + abs(h * dy)
    cx, cy = w / 2, h / 2
    sx, sy = cx - dx * length / 2, cy - dy * length / 2
    ex, ey = cx + dx * length / 2, cy + dy * length / 2
    # Width handle: perpendicular, same length (unused by linear gradients)
    wx, wy = sx - (ey - sy), sy + (ex - sx)
    stops, hints = parse_stops(args, length)
    if hints:
        notes.append("Colour hints (midpoints) are ignored")
    if not stops:
        return None, notes + ["linear-gradient without colour stops"]
    return {"type": "linear", "stops": stops, "handles": _handles(sx, sy, ex, ey, wx, wy, w, h)}, notes


def _handles(sx, sy, ex, ey, wx, wy, w, h) -> list[float]:
    w = max(w, 1e-6)
    h = max(h, 1e-6)
    return [r4(sx / w), r4(sy / h), r4(ex / w), r4(ey / h), r4(wx / w), r4(wy / h)]


def radial_gradient(args: list[str], w: float, h: float) -> tuple[dict | None, list[str]]:
    notes = []
    shape, size, at = "ellipse", "farthest-corner", "center"
    explicit: list[str] = []
    if args and not is_color_token(args[0]):
        config = args.pop(0).strip().lower()
        if " at " in f" {config} ":
            head, _, at = f" {config} ".partition(" at ")
            config = head.strip()
            at = at.strip() or "center"
        for word in config.split():
            if word in ("circle", "ellipse"):
                shape = word
            elif word in ("closest-side", "closest-corner", "farthest-side", "farthest-corner"):
                size = word
            else:
                explicit.append(word)
        if explicit and shape == "ellipse" and len(explicit) == 1:
            shape = "circle"
    cx, cy = parse_bg_position(at, w, h)
    sides_x = (cx, w - cx)
    sides_y = (cy, h - cy)
    if explicit:
        rx = parse_position(explicit[0], 1.0) if explicit[0].endswith("%") else px(explicit[0])
        if explicit[0].endswith("%"):
            rx = rx * w
        ry = rx
        if len(explicit) > 1:
            ry = (float(explicit[1][:-1]) / 100 * h) if explicit[1].endswith("%") else px(explicit[1])
    else:
        closest = size.startswith("closest")
        pick = min if closest else max
        if size.endswith("side"):
            if shape == "circle":
                rx = ry = pick(sides_x + sides_y)
            else:
                rx, ry = pick(sides_x), pick(sides_y)
        else:
            corners = [(abs(x), abs(y)) for x in (cx, w - cx) for y in (cy, h - cy)]
            corner = (min if closest else max)(corners, key=lambda c: math.hypot(*c))
            if shape == "circle":
                rx = ry = math.hypot(*corner)
            else:
                side_rx, side_ry = pick(sides_x), pick(sides_y)
                ratio = side_rx / side_ry if side_ry > 0 else 1.0
                ry = math.sqrt((corner[0] / ratio) ** 2 + corner[1] ** 2) if ratio > 0 else corner[1]
                rx = ratio * ry
    rx, ry = max(rx or 0.0, 1e-3), max(ry or 0.0, 1e-3)
    stops, hints = parse_stops(args, rx)
    if hints:
        notes.append("Colour hints (midpoints) are ignored")
    if not stops:
        return None, notes + ["radial-gradient without colour stops"]
    return {"type": "radial", "stops": stops, "handles": _handles(cx, cy, cx + rx, cy, cx, cy + ry, w, h)}, notes


def conic_gradient(args: list[str], w: float, h: float) -> tuple[dict | None, list[str]]:
    notes = []
    start, at = 0.0, "center"
    if args and not is_color_token(args[0]):
        config = f" {args.pop(0).strip().lower()} "
        if " at " in config:
            config, _, at = config.partition(" at ")
        m = re.search(r"from\s+(\S+)", config)
        if m and parse_angle(m.group(1)) is not None:
            start = parse_angle(m.group(1))
    cx, cy = parse_bg_position(at.strip() or "center", w, h)
    r = max(min(w, h) / 2, 1.0)
    a = math.radians(start)
    # A axis = the 'from' direction (0deg = up), B axis = 90deg clockwise of it
    ex, ey = cx + math.sin(a) * r, cy - math.cos(a) * r
    wx, wy = cx + math.cos(a) * r, cy + math.sin(a) * r
    stops, hints = parse_stops(args, 360.0, unit_full=100.0)
    if not stops:
        return None, notes + ["conic-gradient without colour stops"]
    return {"type": "angular", "stops": stops, "handles": _handles(cx, cy, ex, ey, wx, wy, w, h)}, notes


def gradient_paint(text: str, w: float, h: float) -> tuple[dict | None, list[str]]:
    m = re.fullmatch(r"\s*(repeating-)?(linear|radial|conic)-gradient\((.*)\)\s*", text, re.S)
    if not m:
        return None, [f"Unsupported background image '{text[:60]}'"]
    args = split_top(m.group(3))
    paint, notes = {"linear": linear_gradient, "radial": radial_gradient, "conic": conic_gradient}[m.group(2)](args, w, h)
    if m.group(1):
        notes.append("Repeating gradient drawn once (not repeated)")
    if paint and len(paint["stops"]) > 4:
        notes.append(f"Gradient has {len(paint['stops'])} stops: UE keeps 4")
    return paint, notes


# =====================================================================================================================
# Raw tree -> dwocanvas/2

def _bool_attr(value: str | None) -> bool | None:
    if value is None:
        return None
    return value.strip().lower() not in ("false", "0", "no", "off")


class Converter:
    def __init__(self, raw: dict, html_path: Path, width: int, height: int, name: str, preview_png: bytes | None,
                 rules: dict | None = None):
        self.raw = raw
        self.html_path = html_path
        self.width = width
        self.height = height
        self.name = name
        self.preview_png = preview_png
        self.rules = rules
        self.lint: list[dict] = []
        self.assets: dict[str, dict] = {}
        self._asset_by_sha: dict[str, str] = {}
        self._ids: set[str] = set()
        self.hashed_ids = 0
        self.element_count = 0
        self.tokens: dict[str, dict] = {}
        self.root_tokens = {k: v for k, v in (raw.get("tokens") or {}).items()}
        self._preview_rgba = None
        self.loaded_fonts = {f.get("family", "").strip("\"'") for f in raw.get("fonts") or [] if f.get("status") == "loaded"}
        self._warned_fonts: set[str] = set()

    # --- lint / ids / assets ------------------------------------------------------------------------------------

    def note(self, level: str, message: str, node: dict | None = None, rule: str = "html-adapter") -> None:
        self.lint.append({"level": level, "node": str(node.get("id", "")) if node else "", "name": str(node.get("name", "")) if node else "",
                          "message": message, "rule": rule})

    def unique_id(self, base: str) -> str:
        candidate = base
        n = 2
        while candidate in self._ids:
            candidate = f"{base}~{n}"
            n += 1
        if candidate != base:
            self.note("warning", f"Duplicate id '{base}': renamed to '{candidate}'", {"id": candidate, "name": ""}, "html-duplicate-id")
        self._ids.add(candidate)
        return candidate

    def element_id(self, raw: dict) -> str:
        attrs = raw.get("attrs") or {}
        self.element_count += 1
        explicit = attrs.get("data-db-id") or attrs.get("id")
        if explicit:
            return self.unique_id(explicit)
        self.hashed_ids += 1
        return self.unique_id("h" + hashlib.sha1(raw.get("path", "").encode("utf-8")).hexdigest()[:10])

    def add_asset(self, data: bytes, prefix: str = "img") -> str:
        sha = hashlib.sha256(data).hexdigest()
        if sha in self._asset_by_sha:
            return self._asset_by_sha[sha]
        asset_id = f"{prefix}_{sha[:10]}"
        self.assets[asset_id] = {"data": base64.b64encode(data).decode("ascii"), "sha256": sha, "scale": 1}
        self._asset_by_sha[sha] = asset_id
        return asset_id

    def load_url(self, url: str, node: dict) -> bytes | None:
        parsed = urllib.parse.urlparse(url)
        try:
            if parsed.scheme == "file":
                return Path(urllib.request.url2pathname(parsed.path if not parsed.netloc else "//" + parsed.netloc + parsed.path)).read_bytes()
            if parsed.scheme == "data":
                header, _, payload = url.partition(",")
                return base64.b64decode(payload) if ";base64" in header else urllib.parse.unquote_to_bytes(payload)
        except (OSError, ValueError) as e:
            self.note("warning", f"Image '{url[:80]}' could not be read: {e}", node, "html-image")
            return None
        self.note("warning", f"Image '{url[:80]}' is not a local file: not embedded (download it next to the page)", node, "html-image")
        return None

    def preview_crop(self, rect: dict) -> bytes | None:
        if not self.preview_png:
            return None
        if self._preview_rgba is None:
            try:
                self._preview_rgba = pngio.decode_png(self.preview_png)
            except ValueError:
                self._preview_rgba = False
        if not self._preview_rgba:
            return None
        w, h, rgba = self._preview_rgba
        x0, y0 = int(math.floor(rect["x"])), int(math.floor(rect["y"]))
        x1, y1 = int(math.ceil(rect["x"] + rect["w"])), int(math.ceil(rect["y"] + rect["h"]))
        if x1 <= x0 or y1 <= y0:
            return None
        return pngio.encode_png(x1 - x0, y1 - y0, pngio.crop_rgba(w, h, rgba, x0, y0, x1 - x0, y1 - y0))

    # --- tokens -------------------------------------------------------------------------------------------------

    def token_for(self, raw: dict, key: str, color: list[float] | None) -> str | None:
        """Token path for a colour that came from var(--x), when the token's value is that colour."""
        ref = (raw.get("tokens") or {}).get(key)
        if not ref or ref not in self.root_tokens or not color:
            return None
        value = parse_color(self.root_tokens[ref])
        if not same_color(value, color):
            return None
        name = ref[2:]
        self.tokens.setdefault("color", {})[name] = {"$value": color_hex(value), "$type": "color"}
        return f"color.{name}"

    # --- visuals ------------------------------------------------------------------------------------------------

    def fills(self, raw: dict, node: dict, w: float, h: float, skip_color: bool = False) -> list[dict]:
        style = raw["style"]
        out: list[dict] = []
        color = parse_color(style.get("backgroundColor"))
        if color and color[3] > 0 and not skip_color:
            paint = {"type": "solid", "color": color}
            token = self.token_for(raw, "background", color)
            if token:
                paint["colorToken"] = token
            out.append(paint)
        images = style.get("backgroundImage") or "none"
        if images != "none":
            layers = split_top(images)
            sizes = split_top(style.get("backgroundSize") or "auto")
            repeats = split_top(style.get("backgroundRepeat") or "repeat")
            for i in reversed(range(len(layers))):  # CSS lists the top layer first
                paint = self.layer_paint(layers[i], sizes[i % len(sizes)], repeats[i % len(repeats)], w, h, node)
                if paint:
                    out.append(paint)
        if raw.get("image"):
            paint = self.img_paint(raw, node, w, h)
            if paint:
                out.append(paint)
        if style.get("backgroundClip") not in (None, "border-box") and out:
            self.note("info", f"background-clip: {style.get('backgroundClip')} is drawn as border-box", node)
        return out

    def layer_paint(self, layer: str, size: str, repeat: str, w: float, h: float, node: dict) -> dict | None:
        m = re.fullmatch(r'\s*url\(\s*["\']?(.*?)["\']?\s*\)\s*', layer, re.S)
        if not m:
            paint, notes = gradient_paint(layer, w, h)
            for n in notes:
                self.note("warning" if paint is None else "info", n, node, "html-gradient")
            return paint
        data = self.load_url(m.group(1), node)
        if data is None:
            return None
        natural = pngio.image_size(data)
        paint = {"type": "image", "image": self.add_asset(data)}
        size = size.strip()
        repeat = repeat.strip()
        no_repeat = repeat in ("no-repeat", "no-repeat no-repeat")
        if size == "cover":
            paint["scaleMode"] = "fill"
        elif size == "contain":
            paint["scaleMode"] = "fit"
        elif not no_repeat:
            paint["scaleMode"] = "tile"
            first = size.split()[0] if size else "auto"
            if natural and first != "auto":
                tile_w = (float(first[:-1]) / 100 * w) if first.endswith("%") else px(first, natural[0])
                if natural[0] > 0 and abs(tile_w - natural[0]) > 0.01:
                    paint["scalingFactor"] = r4(tile_w / natural[0])
        else:
            parts = size.split()
            sized = len(parts) == 2 and all(p.endswith("%") or px(p, None) is not None for p in parts)
            covers = sized and all((p == "100%") or abs(px(p, -1) - d) < 0.5 for p, d in zip(parts, (w, h)))
            paint["scaleMode"] = "fill"
            if not covers:
                self.note("info", f"background-size '{size}' with no-repeat approximated as cover", node, "html-image")
        return paint

    def img_paint(self, raw: dict, node: dict, w: float, h: float) -> dict | None:
        image = raw["image"]
        data = self.load_url(image.get("src") or "", node)
        if data is None:
            return None
        fit = raw["style"].get("objectFit") or "fill"
        mode = {"cover": "fill", "contain": "fit", "scale-down": "fit", "none": "crop"}.get(fit, "fill")
        nw, nh = image.get("naturalWidth") or 0, image.get("naturalHeight") or 0
        if fit == "fill" and nw and nh and w and h and abs(nw / nh - w / h) > 0.01:
            self.note("info", "object-fit: fill (stretch) has no schema equivalent: drawn as cover", node, "html-image")
        if (raw["style"].get("objectPosition") or "50% 50%") not in ("50% 50%", "center center", "center"):
            self.note("info", f"object-position '{raw['style'].get('objectPosition')}' is drawn centred", node, "html-image")
        return {"type": "image", "image": self.add_asset(data), "scaleMode": mode}

    def stroke(self, raw: dict, node: dict) -> dict | None:
        style = raw["style"]
        sides = ["Left", "Top", "Right", "Bottom"]
        widths, colors, styles = [], [], []
        for s in sides:
            st = style.get(f"border{s}Style") or "none"
            wv = px(style.get(f"border{s}Width")) if st not in ("none", "hidden") else 0.0
            widths.append(wv or 0.0)
            colors.append(parse_color(style.get(f"border{s}Color")))
            styles.append(st)
        if max(widths) <= 0:
            return None
        used = [c for c, wv in zip(colors, widths) if wv > 0 and c]
        if not used:
            return None
        color = used[0]
        if any(not same_color(c, color) for c in used):
            self.note("warning", "Border sides have different colours: using the first one", node, "html-border")
        out: dict = {"color": color, "width": r2(max(widths)), "align": "inside"}
        token = self.token_for(raw, "border", color)
        if token:
            out["colorToken"] = token
        if len(set(widths)) > 1:
            out["sides"] = [r2(v) for v in widths]
        kinds = {s for s, wv in zip(styles, widths) if wv > 0}
        if kinds & {"dashed", "dotted"}:
            wv = max(widths)
            out["dash"] = [r2(wv * 3), r2(wv * 3)] if "dashed" in kinds else [r2(wv), r2(wv)]
            self.note("info", "Dashed / dotted border approximated with a dash pattern", node, "html-border")
        elif kinds - {"solid"}:
            self.note("info", f"Border style {sorted(kinds - {'solid'})} drawn solid", node, "html-border")
        return out

    def radius(self, raw: dict, w: float, h: float, node: dict) -> list[float] | None:
        style = raw["style"]
        values = []
        for key in ("borderTopLeftRadius", "borderTopRightRadius", "borderBottomRightRadius", "borderBottomLeftRadius"):
            parts = (style.get(key) or "0px").split()
            if len(parts) > 1 and parts[0] != parts[1]:
                self.note("info", f"Elliptical corner radius '{style.get(key)}': using the horizontal radius", node, "html-radius")
            first = parts[0]
            values.append(float(first[:-1]) / 100 * w if first.endswith("%") else (px(first) or 0.0))
        if max(values) <= 0:
            return None
        tl, tr, br, bl = values
        f = 1.0
        for total, side in ((tl + tr, w), (bl + br, w), (tl + bl, h), (tr + br, h)):
            if total > 0 and side > 0:
                f = min(f, side / total)
        return [r2(v * f) for v in values]

    def effects(self, raw: dict, node: dict) -> list[dict]:
        style = raw["style"]
        out = []
        shadows = parse_shadows(style.get("boxShadow"))
        drops = [s for s in shadows if not s["inset"]]
        insets = [s for s in shadows if s["inset"]]
        for kind, group in (("drop-shadow", drops), ("inner-shadow", insets)):
            if len(group) > 1:
                self.note("warning", f"{len(group)} {kind}s: UE draws only the first (topmost)", node, "html-shadow")
            for s in group:
                effect = {"type": kind, "visible": True, "color": s["color"], "offset": s["offset"], "blur": s["blur"], "spread": s["spread"]}
                if kind == "drop-shadow":
                    effect["showBehind"] = False
                out.append(effect)
        blur = parse_filter_blur(style.get("backdropFilter"))
        if blur:
            out.append({"type": "background-blur", "visible": True, "radius": r2(blur)})
        layer = parse_filter_blur(style.get("filter"))
        if layer:
            out.append({"type": "layer-blur", "visible": True, "radius": r2(layer)})
        flt = style.get("filter") or "none"
        if flt != "none" and re.sub(r"blur\([^)]*\)", "", flt).strip():
            self.note("warning", f"filter '{flt}' is not supported (only blur)", node, "html-filter")
        return out

    # --- text ---------------------------------------------------------------------------------------------------

    def line_height(self, style: dict, info: dict) -> float:
        value = px(style.get("lineHeight"), None)
        if value is not None:
            return value
        lines = max(info.get("lines") or 1, 1)
        return info.get("contentHeight") or (info["rect"]["h"] / lines)

    def text_object(self, raw: dict, style: dict, content: str, node: dict) -> dict:
        family = family_name(style.get("fontFamily"))
        if family and family not in self.loaded_fonts and family not in self._warned_fonts:
            self._warned_fonts.add(family)
            self.note("info", f"Font family '{family}' is not a loaded web font (system / generic font): UE needs a matching font rule", node, "html-font")
        try:
            weight = int(float(style.get("fontWeight") or 400))
        except ValueError:
            weight = {"bold": 700, "normal": 400, "lighter": 300, "bolder": 700}.get(style.get("fontWeight"), 400)
        italic = (style.get("fontStyle") or "normal") != "normal"
        t: dict = {
            "content": content,
            "font": {"family": family, "weight": weight, "italic": italic, "size": r2(px(style.get("fontSize"), 16.0)),
                     "style": style_name(weight, italic)},
        }
        color = parse_color(style.get("color"))
        if color:
            t["color"] = color
            token = self.token_for(raw, "color", color)
            if token:
                t["colorToken"] = token
        lh = px(style.get("lineHeight"), None)
        t["lineHeight"] = {"unit": "px", "value": r2(lh)} if lh is not None else {"unit": "auto"}
        ls = px(style.get("letterSpacing"), None)
        if ls:
            t["letterSpacing"] = {"unit": "px", "value": r2(ls)}
        align = (style.get("textAlign") or "start").replace("-webkit-", "")
        t["align"] = {"center": "center", "right": "right", "end": "right", "justify": "justify"}.get(align, "left")
        t["valign"] = "top"
        case = {"uppercase": "upper", "lowercase": "lower", "capitalize": "title"}.get(style.get("textTransform") or "none")
        if case:
            t["case"] = case
        deco = style.get("textDecorationLine") or "none"
        if "underline" in deco:
            t["decoration"] = "underline"
        elif "line-through" in deco:
            t["decoration"] = "strike"
        return t

    def text_effects(self, style: dict) -> list[dict]:
        return [{"type": "drop-shadow", "visible": True, "color": s["color"], "offset": s["offset"], "blur": s["blur"], "spread": 0}
                for s in parse_shadows(style.get("textShadow"))][:1]

    def truncation(self, style: dict) -> tuple[bool, int]:
        ellipsis = style.get("textOverflow") == "ellipsis" and (style.get("overflowX") or "visible") != "visible"
        clamp = style.get("webkitLineClamp") or "none"
        lines = int(clamp) if clamp.isdigit() else 0
        return ellipsis, lines

    # --- nodes --------------------------------------------------------------------------------------------------

    def base_node(self, raw: dict, node_id: str, name: str, box: dict) -> dict:
        attrs = raw.get("attrs") or {}
        node: dict = {"id": node_id, "name": name, "type": "frame"}
        if attrs.get("data-role"):
            node["role"] = attrs["data-role"].strip()
        variable = _bool_attr(attrs.get("data-variable"))
        if variable is not None:
            node["variable"] = variable
        if attrs.get("data-widget-name"):
            widget = naming.sanitize_widget_name(attrs["data-widget-name"])
            if widget != attrs["data-widget-name"]:
                self.note("warning", f"data-widget-name '{attrs['data-widget-name']}' is not a UE identifier: using '{widget}'", node, "html-naming")
            node["widgetName"] = widget
        node["box"] = box
        return node

    def element_name(self, raw: dict) -> str:
        attrs = raw.get("attrs") or {}
        if attrs.get("data-name"):
            return attrs["data-name"]
        if attrs.get("id"):
            return attrs["id"]
        classes = (attrs.get("class") or "").split()
        return raw["tag"] + ("." + classes[0] if classes else "")

    def box_of(self, raw: dict, origin: tuple[float, float]) -> tuple[dict, float]:
        """Unrotated box relative to ``origin`` and the rotation (degrees, counter-clockwise)."""
        rect = raw["rect"]
        x, y, w, h = rect["x"], rect["y"], rect["w"], rect["h"]
        rotation = 0.0
        transform = raw.get("transform")
        if transform:
            m = re.fullmatch(r"matrix\(([^)]*)\)", transform["matrix"].strip())
            if m:
                a, b, c, d, e, f = (float(v) for v in m.group(1).split(","))
                rotation = -math.degrees(math.atan2(b, a))
                sx = math.hypot(a, b)
                sy = (a * d - b * c) / sx if sx else 0.0
                if abs(sx - 1) > 0.01 or abs(sy - 1) > 0.01:
                    self.note("warning", f"transform scale ({sx:.2f}, {sy:.2f}) is not supported: ignored", {"id": raw.get("path", ""), "name": self.element_name(raw)}, "html-transform")
                x, y = transform["cx"] - w / 2, transform["cy"] - h / 2
            else:
                self.note("warning", f"transform '{transform['matrix'][:40]}' (3D) is not supported: ignored", {"id": raw.get("path", ""), "name": self.element_name(raw)}, "html-transform")
        return {"x": r2(x - origin[0]), "y": r2(y - origin[1]), "w": r2(max(w, 0)), "h": r2(max(h, 0))}, r2(rotation)

    def has_visual(self, node: dict) -> bool:
        return bool(node.get("fills") or node.get("stroke") or node.get("effects"))

    def convert_element(self, raw: dict, origin: tuple[float, float], parent_mode: str, parent_raw: dict | None) -> dict | None:
        style = raw["style"]
        node_id = self.element_id(raw)
        name = self.element_name(raw)
        box, rotation = self.box_of(raw, origin)
        node = self.base_node(raw, node_id, name, box)
        w, h = box["w"], box["h"]
        own_origin = (raw["rect"]["x"], raw["rect"]["y"])

        if style.get("visibility") == "hidden":
            node["visible"] = False
        opacity = float(style.get("opacity") or 1)
        if opacity < 1:
            node["opacity"] = r4(opacity)
        if rotation:
            node["rotation"] = rotation
        if (style.get("overflowX") or "visible") != "visible" or (style.get("overflowY") or "visible") != "visible":
            node["clip"] = True
        if raw.get("pseudo"):
            self.note("warning", f"{'/'.join(raw['pseudo'])} content is not extracted: use real elements", node, "html-pseudo")
        blend = style.get("mixBlendMode") or "normal"
        if blend != "normal":
            node["blendMode"] = blend

        # Position in the parent
        flow_parent = parent_mode in ("horizontal", "vertical", "wrap", "grid")
        absolute = style.get("position") in ("absolute", "fixed")
        if flow_parent and absolute:
            node["positioning"] = "absolute"
        sizing = raw.get("sizing")
        if flow_parent and not absolute and parent_mode != "grid" and sizing:
            node["sizing"] = {"h": sizing["h"], "v": sizing["v"]}
        else:
            node["sizing"] = {"h": "fixed", "v": "fixed"}
        if flow_parent and not absolute and parent_raw is not None:
            margins = [px(style.get(k), 0.0) for k in ("marginLeft", "marginTop", "marginRight", "marginBottom")]
            if any(abs(v or 0) > 0.01 for v in margins):
                self.note("info", "Margins inside a flex / grid container are not represented (spacing comes from gap / padding)", node, "html-margin")

        # Visuals
        fills = self.fills(raw, node, w, h, skip_color=raw.get("_canvasPaint", False))
        if fills:
            node["fills"] = fills
        stroke = self.stroke(raw, node)
        if stroke:
            node["stroke"] = stroke
        radius = self.radius(raw, w, h, node)
        if radius:
            node["radius"] = radius
        effects = self.effects(raw, node)
        if effects:
            node["effects"] = effects

        # Replaced elements without a schema equivalent: bake from the preview
        if raw.get("replaced"):
            node["type"] = "rectangle"
            if not node.get("role"):
                image = self.preview_crop(raw["rect"])
                if image:
                    node["bake"] = {"image": self.add_asset(image, "bake")}
                    self.note("info", f"<{raw['tag']}> baked from the preview render", node, "html-bake")
                else:
                    self.note("warning", f"<{raw['tag']}> is not supported and no preview is available: drawn empty", node, "html-unsupported")
            return node
        if raw.get("image"):
            node["type"] = "rectangle"
            return node

        # Text-only element
        if raw.get("text"):
            return self.convert_text_element(raw, node, parent_mode, flow_parent, own_origin)

        # Container
        children_raw = raw.get("children") or []
        layout = self.layout(raw, node, children_raw)
        if layout:
            node["layout"] = layout
        mode = layout["mode"] if layout else "none"
        children = []
        for i, child_raw in enumerate(children_raw):
            if child_raw.get("kind") == "text":
                child = self.convert_run(child_raw, node, raw, own_origin, mode, i)
            else:
                child = self.convert_element(child_raw, own_origin, mode, raw)
            if child:
                children.append(child)
        if children:
            node["children"] = children
            if layout and layout["mode"] in ("horizontal", "vertical"):
                self.fix_cross_align(node, layout)
                self.measure_gap(node, layout, raw)
        elif self.has_visual(node):
            node["type"] = "rectangle"
        return node

    def layout(self, raw: dict, node: dict, children: list[dict]) -> dict | None:
        style = raw["style"]
        display = style.get("display") or "block"
        padding = [r2(px(style.get(k)) or 0.0) for k in ("paddingLeft", "paddingTop", "paddingRight", "paddingBottom")]
        strokes = bool(node.get("stroke"))
        if display in ("flex", "inline-flex"):
            direction = style.get("flexDirection") or "row"
            row = direction.startswith("row")
            reverse = direction.endswith("reverse")
            wrap = (style.get("flexWrap") or "nowrap") != "nowrap"
            if wrap and not row:
                self.note("warning", "Column flex-wrap is not supported: laid out as a single column", node, "html-flex")
                wrap = False
            if reverse:
                children.reverse()
                self.note("info", f"flex-direction: {direction}: children listed in visual order", node, "html-flex")
            col_gap = px(style.get("columnGap"), 0.0) or 0.0
            row_gap = px(style.get("rowGap"), 0.0) or 0.0
            justify = style.get("justifyContent") or "normal"
            main = {"center": "center", "space-between": "space-between", "flex-end": "end", "end": "end", "right": "end"}.get(justify, "start")
            if reverse and main in ("start", "end") and justify not in ("normal",):
                main = "end" if main == "start" else "start"
            elif reverse and justify in ("normal", "flex-start"):
                main = "end"
            if justify in ("space-around", "space-evenly"):
                main = "center" if len(children) <= 1 else "space-between"
                self.note("info", f"justify-content: {justify} approximated as {main}", node, "html-flex")
            align = style.get("alignItems") or "normal"
            cross = {"normal": "stretch", "stretch": "stretch", "center": "center", "flex-end": "end", "end": "end", "self-end": "end"}.get(align, "start")
            if "baseline" in align:
                self.note("info", "align-items: baseline approximated as start", node, "html-flex")
            layout = {"mode": "wrap" if wrap else ("horizontal" if row else "vertical"),
                      "gap": r2(col_gap if row else row_gap), "padding": padding, "mainAlign": main, "crossAlign": cross}
            if wrap:
                layout["crossGap"] = r2(row_gap)
            if strokes:
                layout["strokesIncluded"] = True
            return layout
        if display in ("grid", "inline-grid"):
            template = re.sub(r"\[[^\]]*\]", " ", style.get("gridTemplateColumns") or "none").split()
            columns = max(len(template), 1) if template != ["none"] else 1
            if (style.get("gridAutoFlow") or "row").startswith("column"):
                self.note("warning", "grid-auto-flow: column is laid out row by row", node, "html-grid")
            layout = {"mode": "grid", "columns": columns, "gap": r2(px(style.get("columnGap"), 0.0) or 0.0),
                      "crossGap": r2(px(style.get("rowGap"), 0.0) or 0.0), "padding": padding}
            if strokes:
                layout["strokesIncluded"] = True
            return layout
        if children:
            return {"mode": "none"}
        return None

    def fix_cross_align(self, node: dict, layout: dict) -> None:
        """CSS stretch only stretches auto-sized items: keep 'stretch' when every flow child fills the cross axis."""
        if layout.get("crossAlign") != "stretch":
            return
        axis = "v" if layout["mode"] == "horizontal" else "h"
        flow = [c for c in node.get("children", []) if c.get("positioning") != "absolute"]
        if any((c.get("sizing") or {}).get(axis) != "fill" for c in flow):
            layout["crossAlign"] = "start"

    def measure_gap(self, node: dict, layout: dict, raw: dict) -> None:
        """Spacing actually rendered between flow children (gap + margins); flags uneven spacing."""
        if layout.get("mainAlign") == "space-between":
            return
        horizontal = layout["mode"] == "horizontal"
        flow = [c for c in node.get("children", []) if c.get("positioning") != "absolute"]
        if len(flow) < 2:
            return
        gaps = []
        for a, b in zip(flow, flow[1:]):
            if horizontal:
                gaps.append(b["box"]["x"] - (a["box"]["x"] + a["box"]["w"]))
            else:
                gaps.append(b["box"]["y"] - (a["box"]["y"] + a["box"]["h"]))
        if max(gaps) - min(gaps) <= 0.6:
            measured = r2(sum(gaps) / len(gaps))
            if abs(measured - layout["gap"]) > 0.6:
                self.note("info", f"Gap {layout['gap']} + margins renders as {measured}px: using {measured}", node, "html-flex")
                layout["gap"] = measured
        else:
            self.note("warning", f"Uneven spacing between children ({', '.join(str(r2(g)) for g in gaps)}): gap {layout['gap']} used", node, "html-flex")

    def text_autoresize(self, style: dict, info: dict, sizing: dict, flow_parent: bool, content_w: float) -> tuple[str, int]:
        ellipsis, clamp = self.truncation(style)
        if ellipsis:
            return "truncate", 0
        if clamp:
            return "height", clamp
        nowrap = (style.get("whiteSpace") or "normal") in ("nowrap", "pre")
        if flow_parent:
            return ("width" if sizing.get("h") == "hug" else "height"), 0
        if info.get("lines", 1) <= 1 and (nowrap or abs(info["rect"]["w"] - content_w) <= 1.0):
            return "width", 0
        return "height", 0

    def convert_text_element(self, raw: dict, node: dict, parent_mode: str, flow_parent: bool, own_origin: tuple[float, float]) -> dict:
        style = raw["style"]
        info = raw["text"]
        if info.get("mixed"):
            self.note("warning", "Text has mixed inline styles: flattened to the element's style", node, "html-text")
        padding = [px(style.get(k)) or 0.0 for k in ("paddingLeft", "paddingTop", "paddingRight", "paddingBottom")]
        border = [px(style.get(f"border{s}Width")) or 0.0 for s in ("Left", "Top", "Right", "Bottom")]
        role = node.get("role") or (naming.match_prefix(node["name"], self.rules or naming.default_rules()) or {}).get("role")
        ellipsis, clamp = self.truncation(style)
        # overflow: hidden is part of an ellipsis / line clamp (the text block clips itself)
        clips = node.get("clip") and not (ellipsis or clamp)
        plain = not self.has_visual(node) and not any(padding) and not clips and role in (None, "auto")
        t = self.text_object(raw, style, info["content"], node)

        if plain:
            node["type"] = "text"
            node.pop("clip", None)
            mode, lines = self.text_autoresize(style, info, node["sizing"], flow_parent, node["box"]["w"])
            t["autoResize"] = mode
            if lines:
                t["maxLines"] = lines
            node["text"] = t
            fx = self.text_effects(style)
            if fx:
                node["effects"] = fx
            return node

        # Element with its own visuals: a frame laying out one text child (centring done by UE, not by offsets)
        content_x = border[0] + padding[0]
        content_w = node["box"]["w"] - content_x - border[2] - padding[2]
        lh = self.line_height(style, info)
        lines = max(info.get("lines") or 1, 1)
        content_h = info.get("contentHeight") or lh
        text_y = info["rect"]["y"] - own_origin[1] - (lh - content_h) / 2
        single = lines <= 1 and not ellipsis and not clamp
        child_box = {"x": r2(content_x if not single else info["rect"]["x"] - own_origin[0]), "y": r2(text_y),
                     "w": r2(content_w if not single else info["rect"]["w"]), "h": r2(lh * lines)}
        child = {"id": self.unique_id(node["id"] + "#text"), "name": "Label", "type": "text", "box": child_box,
                 "sizing": {"h": "hug" if single else "fill", "v": "hug"}}
        if ellipsis:
            t["autoResize"] = "truncate"
        else:
            t["autoResize"] = "width" if single else "height"
        if clamp:
            t["maxLines"] = clamp
        child["text"] = t
        fx = self.text_effects(style)
        if fx:
            child["effects"] = fx
        node["layout"] = {"mode": "horizontal", "gap": 0, "padding": [r2(v) for v in padding],
                          "mainAlign": {"center": "center", "right": "end"}.get(t["align"], "start"), "crossAlign": "start"}
        if node.get("stroke"):
            node["layout"]["strokesIncluded"] = True
        node["children"] = [child]
        return node

    def convert_run(self, raw: dict, parent: dict, parent_raw: dict, origin: tuple[float, float], parent_mode: str, index: int) -> dict:
        """Anonymous text run beside elements (an anonymous flex item / block)."""
        style = parent_raw["style"]
        info = raw["text"]
        lh = self.line_height(style, info)
        lines = max(info.get("lines") or 1, 1)
        content_h = info.get("contentHeight") or lh
        rect = info["rect"]
        box = {"x": r2(rect["x"] - origin[0]), "y": r2(rect["y"] - origin[1] - (lh - content_h) / 2), "w": r2(rect["w"]), "h": r2(lh * lines)}
        node = {"id": self.unique_id(f"{parent['id']}#t{index}"), "name": "Text", "type": "text", "box": box}
        flow = parent_mode in ("horizontal", "vertical", "wrap", "grid")
        node["sizing"] = {"h": "hug" if lines <= 1 else ("fill" if flow else "fixed"), "v": "hug" if flow else "fixed"}
        t = self.text_object(parent_raw, style, info["content"], node)
        t["autoResize"] = "width" if lines <= 1 else "height"
        node["text"] = t
        return node

    # --- document -----------------------------------------------------------------------------------------------

    def canvas_color(self) -> tuple[list[float], bool]:
        """Viewport background (html, else body, else white) and whether it came from the body."""
        html = parse_color((self.raw.get("canvas") or {}).get("html"))
        if html and html[3] > 0:
            return html, False
        body = parse_color(self.raw["body"]["style"].get("backgroundColor"))
        if body and body[3] > 0:
            if body[3] < 1:
                a = body[3]
                body = [r4(c * a + (1 - a)) for c in body[:3]] + [1.0]
            return body, True
        return [1.0, 1.0, 1.0, 1.0], False

    def convert(self) -> dict:
        body = self.raw["body"]
        bg, from_body = self.canvas_color()
        if from_body:
            body["_canvasPaint"] = True  # CSS propagates the body background to the canvas
        body_node = self.convert_element(body, (0.0, 0.0), "none", None)
        root_id = self.unique_id("root") if body_node["id"] != "root" else "root~doc"
        root = {"id": root_id, "name": self.name, "type": "frame",
                "box": {"x": 0, "y": 0, "w": self.width, "h": self.height}, "sizing": {"h": "fixed", "v": "fixed"},
                "clip": True, "fills": [{"type": "solid", "color": bg}]}
        if bg == [1.0, 1.0, 1.0, 1.0] and not from_body:
            self.note("info", "Page has no background colour: the browser's white canvas is used", root, "html-canvas")

        body_box = body_node["box"]
        at_origin = abs(body_box["x"]) < 0.5 and abs(body_box["y"]) < 0.5
        fills_viewport = at_origin and abs(body_box["w"] - self.width) < 0.5 and abs(body_box["h"] - self.height) < 0.5
        layout = body_node.get("layout") or {"mode": "none"}
        plain_body = not self.has_visual(body_node) and not body_node.get("role") and body_node.get("type") != "text"
        body_explicit = bool((body.get("attrs") or {}).get("data-db-id") or (body.get("attrs") or {}).get("id"))
        if plain_body and (layout["mode"] == "none" or fills_viewport):
            # The body node itself is dropped: it does not count against stable ids
            self.element_count -= 1
            self.hashed_ids -= 0 if body_explicit else 1
        if plain_body and layout["mode"] == "none":
            # Flatten: body children go straight into the root (boxes shift by the body offset)
            for child in body_node.get("children", []):
                child["box"]["x"] = r2(child["box"]["x"] + body_box["x"])
                child["box"]["y"] = r2(child["box"]["y"] + body_box["y"])
            root["children"] = body_node.get("children", [])
            self._ids.discard(body_node["id"])
        elif plain_body and fills_viewport:
            # The body is the screen's layout
            root["layout"] = layout
            root["children"] = body_node.get("children", [])
        else:
            root["children"] = [body_node]
        if not root.get("children"):
            root.pop("children", None)

        doc_id = naming.sanitize_widget_name(self.name)
        document = {"id": doc_id, "name": self.name, "kind": "screen", "root": root}
        if self.preview_png:
            preview_id = "preview"
            self.assets[preview_id] = {"data": base64.b64encode(self.preview_png).decode("ascii"),
                                       "sha256": hashlib.sha256(self.preview_png).hexdigest(), "scale": 1}
            document["preview"] = preview_id

        caps = [c for c in CAPABILITIES if c != "preview" or self.preview_png]
        if self.hashed_ids == 0:
            caps.append("stable-ids")
        else:
            self.note("info", f"{self.hashed_ids} of {self.element_count} element(s) have no data-db-id / id: their ids are DOM-path hashes "
                              "(re-import identity is not guaranteed when the page structure changes)", None, "stable-ids")
        if self.tokens:
            caps.append("tokens")

        doc: dict = {"schema": "dwocanvas/2",
                     "source": {"tool": "html", "adapter": ADAPTER, "mode": "extract", "file": self.html_path.name},
                     "capabilities": caps}
        if self.tokens:
            doc["tokens"] = self.tokens
        doc["assets"] = self.assets
        doc["documents"] = [document]
        naming.apply_naming(doc, self.rules)
        for font in self.raw.get("fonts") or []:
            if font.get("status") == "error":
                self.note("warning", f"Web font '{font.get('family')}' {font.get('weight')} {font.get('style')} failed to load: the preview uses a fallback font", None, "html-font")
        for warning in self.raw.get("warnings") or []:
            self.note("info", warning, None, "html-extractor")
        doc["lint"] = self.lint
        return doc


# =====================================================================================================================
# API

def convert_raw(raw: dict, html_path: str | os.PathLike, width: int, height: int, name: str, preview_png: bytes | None,
                rules: dict | None = None) -> dict:
    return Converter(raw, Path(html_path), width, height, name, preview_png, rules).convert()


def resolve_options(html_path: Path, width: int | None, height: int | None, name: str | None) -> tuple[int, int, str]:
    meta = page_meta(read_html(html_path))
    if (width is None or height is None) and "viewport" in meta:
        m = re.fullmatch(r"\s*(\d+)\s*[xX,]\s*(\d+)\s*", meta["viewport"])
        if m:
            width = width or int(m.group(1))
            height = height or int(m.group(2))
    width = width or DEFAULT_WIDTH
    height = height or DEFAULT_HEIGHT
    name = name or meta.get("name") or naming.sanitize_widget_name(html_path.name.split(".")[0])
    return width, height, name


def convert_file(html_path: str | os.PathLike, width: int | None = None, height: int | None = None, name: str | None = None,
                 browser: str | None = None, preview: bool = True, rules: dict | None = None, keep_temp: bool = False,
                 raw_out: str | os.PathLike | None = None) -> tuple[dict, list[dict]]:
    """Extract + convert + validate. Returns ``(doc, issues)`` (issues from the shared validator)."""
    html_path = Path(html_path).resolve()
    width, height, name = resolve_options(html_path, width, height, name)
    raw, png, used = extract(html_path, width, height, browser, preview, keep_temp=keep_temp)
    if raw_out:
        Path(raw_out).write_text(json.dumps(raw, indent=1), encoding="utf-8")
    doc = convert_raw(raw, html_path, width, height, name, png, rules)
    doc["source"]["x-html"] = {"browser": Path(used).stem, "viewport": [width, height]}
    issues = validate_module.validate(doc)
    # Validator rule findings join the adapter's lint (schema errors are reported, not stored)
    seen = {(i["rule"], i["node"], i["message"]) for i in doc["lint"]}
    doc["lint"] += [{k: i[k] for k in ("level", "node", "name", "message", "rule")} for i in issues
                    if i.get("rule") != "schema" and (i["rule"], i["node"], i["message"]) not in seen and i["rule"] != "stable-ids"]
    return doc, issues


def write_doc(doc: dict, path: str | os.PathLike) -> None:
    Path(path).write_text(json.dumps(doc, indent=1, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="html2dwocanvas", description="HTML/CSS page -> dwocanvas/2 document (headless Edge / Chrome).")
    parser.add_argument("input", help="HTML file")
    parser.add_argument("-o", "--output", help="output .dwocanvas.json (default: next to the input)")
    parser.add_argument("--width", type=int, help=f"viewport width (default: <meta name=dwocanvas:viewport> or {DEFAULT_WIDTH})")
    parser.add_argument("--height", type=int, help=f"viewport height (default: <meta name=dwocanvas:viewport> or {DEFAULT_HEIGHT})")
    parser.add_argument("--name", help="document name (default: <meta name=dwocanvas:name> or the file name)")
    parser.add_argument("--browser", help="Chromium executable (default: $DWOCANVAS_BROWSER, Edge, then Chrome)")
    parser.add_argument("--rules", help="naming rules JSON (default: the shared naming.json)")
    parser.add_argument("--no-preview", action="store_true", help="do not capture the preview screenshot")
    parser.add_argument("--raw", help="also write the raw extractor JSON here (debugging)")
    parser.add_argument("--keep-temp", action="store_true", help="keep the temporary browser files")
    parser.add_argument("-v", "--verbose", action="store_true", help="also print info lint items")
    args = parser.parse_args(argv)

    html_path = Path(args.input)
    output = Path(args.output) if args.output else html_path.with_name(html_path.name.split(".")[0] + ".dwocanvas.json")
    try:
        rules = naming.load_rules(args.rules) if args.rules else None
        doc, issues = convert_file(html_path, args.width, args.height, args.name, args.browser, not args.no_preview, rules,
                                   args.keep_temp, args.raw)
    except (AdapterError, ValueError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    errors = [i for i in issues if i["level"] == "error"]
    shown = [i for i in doc["lint"] if i["level"] == "warning" or args.verbose] + errors
    for item in shown:
        where = f" {item['node']}" + (f" ({item['name']})" if item.get("name") else "") if item.get("node") else ""
        print(f"{item['level']} [{item['rule']}]{where}: {item['message']}", file=sys.stderr)
    if errors:
        print(f"{len(errors)} validation error(s): {output} not written", file=sys.stderr)
        return 1
    write_doc(doc, output)
    counts = validate_module.count_levels(doc["lint"])
    print(f"wrote {output} ({counts['warning']} warning(s), {counts['info']} info)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

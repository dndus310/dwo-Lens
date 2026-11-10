"""Rebuild the HTML conformance cases in plugin/DwoCanvas/Conformance/html_*/.

    python canvas/adapters/html/build_conformance.py [case ...] [--browser PATH]

For each case folder ``<case>/`` holding ``<case>.html``: (re)generate the test textures the case uses, run the
HTML adapter (viewport and document name come from the page's ``dwocanvas:*`` meta tags) and write
``<case>.dwocanvas.json`` with the preview embedded. The ``x-conformance`` block of an existing JSON file
(maxDiff measured in Unreal, notes) is kept; new cases get the default (maxDiff 0.05).

Standard library only.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import html2dwocanvas  # noqa: E402
import pngio  # noqa: E402

REPO = HERE.parents[2]
CONFORMANCE = REPO / "plugin" / "DwoCanvas" / "Conformance"
DEFAULT_EXTENSION = {"maxDiff": 0.05, "notes": ""}


# ---------------------------------------------------------------------------------------------------------------------
# Test textures (deterministic, smooth so resampling differences between browser and UE stay small)

def _mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b))


def photo_png(width: int = 64, height: int = 48) -> bytes:
    """A small 'landscape': sky gradient, soft sun, a hill with an anti-aliased edge."""
    sky_top, sky_bottom = (40, 92, 160), (150, 196, 235)
    sun = (255, 214, 102)
    hill_top, hill_bottom = (82, 160, 90), (34, 92, 52)
    out = bytearray()
    for y in range(height):
        for x in range(width):
            c = _mix(sky_top, sky_bottom, y / (height - 1))
            d = math.hypot(x + 0.5 - 46, y + 0.5 - 14)
            c = _mix(c, sun, min(max(10.5 - d, 0.0), 1.0))
            ridge = 30 + 6 * math.sin((x + 0.5) / width * math.pi * 2)
            c = _mix(c, _mix(hill_top, hill_bottom, (y - ridge) / (height - ridge) if y > ridge else 0.0),
                     min(max(y + 0.5 - ridge, 0.0), 1.0))
            out += bytes(int(round(v)) for v in c) + b"\xff"
    return pngio.encode_png(width, height, out)


def tile_png(size: int = 16) -> bytes:
    """8 px checker in two blues."""
    a, b = (58, 123, 213), (90, 196, 255)
    out = bytearray()
    for y in range(size):
        for x in range(size):
            out += bytes(a if (x // 8 + y // 8) % 2 == 0 else b) + b"\xff"
    return pngio.encode_png(size, size, out)


TEXTURES = {"html_images": {"photo.png": photo_png, "tile.png": tile_png}}


# ---------------------------------------------------------------------------------------------------------------------

def case_dirs(names: list[str] | None = None) -> list[Path]:
    dirs = sorted(p for p in CONFORMANCE.glob("html_*") if (p / f"{p.name}.html").is_file())
    if names:
        dirs = [p for p in dirs if p.name in names]
    return dirs


def build_case(case: Path, browser: str | None = None) -> tuple[Path, list[dict]]:
    for file_name, make in TEXTURES.get(case.name, {}).items():
        (case / file_name).write_bytes(make())
    out = case / f"{case.name}.dwocanvas.json"
    extension = dict(DEFAULT_EXTENSION)
    if out.is_file():
        try:
            extension = json.loads(out.read_text(encoding="utf-8")).get("x-conformance", extension)
        except (ValueError, OSError):
            pass
    doc, issues = html2dwocanvas.convert_file(case / f"{case.name}.html", browser=browser)
    errors = [i for i in issues if i["level"] == "error"]
    if errors:
        return out, errors
    # Paths in the committed file stay machine-neutral
    doc["x-conformance"] = extension
    html2dwocanvas.write_doc(doc, out)
    return out, [i for i in doc["lint"] if i["level"] == "warning"]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("cases", nargs="*", help="case folder names (default: every html_* case)")
    parser.add_argument("--browser", help="Chromium executable")
    args = parser.parse_args(argv)
    status = 0
    for case in case_dirs(args.cases):
        out, items = build_case(case, args.browser)
        errors = [i for i in items if i["level"] == "error"]
        print(f"{case.name}: {'FAILED' if errors else 'ok'} ({len(items)} {'error' if errors else 'warning'}(s)) -> {out.name}")
        for item in items:
            print(f"  {item['level']} [{item['rule']}] {item.get('node', '')}: {item['message']}")
        status |= 1 if errors else 0
    return status


if __name__ == "__main__":
    sys.exit(main())

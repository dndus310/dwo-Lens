# DwoCanvas - writes Samples/sample_shop.json, a designbridge/1 export that exercises every mapping path.
# Pure Python (no PIL): PNGs are generated with zlib.
#
#   python make_sample.py

import base64
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "Samples", "sample_shop.json")


def png(width, height, pixel):
    rows = b""
    for y in range(height):
        rows += b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def b64(data):
    return base64.b64encode(data).decode("ascii")


def thumb(x, y):
    # warm gradient with a light disc
    r = 230 - y
    g = 120 + x // 2
    b = 60 + y // 3
    d = math.hypot(x - 40, y - 40)
    if d < 18:
        return (255, 240, 200, 255)
    return (max(0, min(255, r)), max(0, min(255, g)), max(0, min(255, b)), 255)


def star(x, y):
    # baked 2x star with soft glow overflow (box 48x48 + 8 overflow each side -> 64x64 @1x -> 128 @2x)
    cx, cy = 64, 64
    dx, dy = x - cx, y - cy
    angle = math.atan2(dy, dx)
    radius = math.hypot(dx, dy)
    edge = 30 + 14 * math.cos(5 * angle)
    if radius < edge:
        return (255, 205, 60, 255)
    glow = max(0.0, 1.0 - (radius - edge) / 16.0)
    return (255, 180, 40, int(120 * glow))


def node(id_, name, type_, x, y, w, h, **extra):
    n = {"id": id_, "name": name, "type": type_, "box": {"x": x, "y": y, "w": w, "h": h}}
    n.update(extra)
    return n


def text(id_, name, x, y, w, h, content, size=16, style="Regular", color=(1, 1, 1, 1), **extra):
    t = {
        "content": content,
        "font": {"family": "Roboto", "style": style, "size": size},
        "color": list(color),
        "autoResize": extra.pop("autoResize", "width"),
    }
    t.update(extra.pop("text", {}))
    return node(id_, name, "text", x, y, w, h, text=t, **extra)


def solid(r, g, b, a=1.0, **extra):
    paint = {"type": "solid", "color": [r, g, b, a]}
    paint.update(extra)
    return paint


item_card = {
    "id": "C:1",
    "name": "ItemCard",
    "root": node("C:1", "ItemCard", "component", 0, 0, 160, 200,
        layout={"mode": "vertical", "gap": 8, "padding": [10, 10, 10, 10], "mainAlign": "start", "crossAlign": "stretch"},
        fills=[solid(0.16, 0.17, 0.21)],
        radius=[12, 12, 12, 12],
        stroke={"color": [1, 1, 1, 0.08], "width": 1, "align": "inside"},
        children=[
            node("C:2", "Thumbnail", "rectangle", 10, 10, 140, 110,
                sizing={"h": "fill", "v": "fixed"},
                fills=[{"type": "image", "image": "img_thumb", "scaleMode": "fill"}],
                radius=[8, 8, 8, 8]),
            text("C:3", "$ItemName", 10, 128, 140, 20, "Crystal Sword", 15, "SemiBold",
                autoResize="truncate", sizing={"h": "fill", "v": "hug"}),
            text("C:4", "$Price", 10, 156, 60, 18, "1,200 G", 13, "Regular", (1.0, 0.8, 0.3, 1)),
        ]),
}

panel_children = [
    text("P:2", "Title", 20, 20, 300, 28, "Daily Offers", 22, "Bold",
        text={"letterSpacing": {"unit": "percent", "value": 2}, "case": "upper"}),
    text("P:3", "Description", 20, 56, 300, 40,
        "Limited items refresh every day at midnight. Long text truncates with an ellipsis when it does not fit.",
        13, "Regular", (0.75, 0.78, 0.85, 1), autoResize="height", sizing={"h": "fill", "v": "hug"},
        text={"lineHeight": {"unit": "percent", "value": 140}, "maxLines": 2}),
    node("P:4", "Swatches", "frame", 20, 110, 300, 64,
        layout={"mode": "horizontal", "gap": 12, "padding": [0, 0, 0, 0], "mainAlign": "space-between", "crossAlign": "center"},
        sizing={"h": "fill", "v": "hug"},
        children=[
            node("P:5", "GradientChip", "rectangle", 0, 0, 64, 64,
                fills=[{"type": "linear", "stops": [{"position": 0, "color": [0.36, 0.55, 1, 1]}, {"position": 1, "color": [0.75, 0.3, 0.95, 1]}],
                        "handles": [0, 0, 1, 1, 0, 1]}],
                radius=[16, 16, 16, 16]),
            node("P:6", "DashedChip", "rectangle", 76, 0, 64, 64,
                fills=[solid(1, 1, 1, 0.04)],
                stroke={"color": [0.6, 0.65, 0.75, 1], "width": 2, "align": "center", "dash": [6, 4]},
                radius=[10, 10, 10, 10]),
            node("P:7", "InsetChip", "rectangle", 152, 0, 64, 64,
                fills=[solid(0.2, 0.22, 0.27)],
                effects=[{"type": "inner-shadow", "color": [0, 0, 0, 0.6], "offset": [0, 3], "blur": 8, "spread": 0}],
                radius=[10, 10, 10, 10]),
            node("P:8", "Dot", "ellipse", 228, 12, 40, 40, fills=[solid(0.3, 0.85, 0.55)]),
        ]),
    node("P:9", "btn_Buy", "instance", 20, 190, 300, 44,
        layout={"mode": "horizontal", "gap": 8, "padding": [16, 0, 16, 0], "mainAlign": "center", "crossAlign": "center"},
        sizing={"h": "fill", "v": "fixed"},
        fills=[solid(0.1, 0.45, 0.95)], radius=[10, 10, 10, 10],
        states={
            "hovered": {"fills": [solid(0.2, 0.55, 1.0)], "radius": [10, 10, 10, 10]},
            "pressed": {"fills": [solid(0.05, 0.35, 0.8)], "radius": [10, 10, 10, 10]},
            "disabled": {"fills": [solid(0.3, 0.32, 0.36)], "radius": [10, 10, 10, 10]},
        },
        children=[text("P:10", "Label", 120, 12, 60, 20, "Buy now", 15, "Bold")]),
]

root = node("S:1", "Shop", "frame", 0, 0, 1280, 720,
    fills=[solid(0.06, 0.07, 0.09)],
    clip=True,
    children=[
        # Header: auto layout, stretched across the screen
        node("S:2", "Header", "frame", 0, 0, 1280, 72,
            constraints={"h": "stretch", "v": "min"},
            layout={"mode": "horizontal", "gap": 16, "padding": [32, 0, 24, 0], "mainAlign": "space-between", "crossAlign": "center"},
            fills=[solid(0.1, 0.11, 0.14)],
            effects=[{"type": "drop-shadow", "color": [0, 0, 0, 0.35], "offset": [0, 4], "blur": 16, "spread": 0}],
            children=[
                text("S:3", "ShopTitle", 32, 22, 120, 28, "Shop", 24, "Bold"),
                node("S:4", "input_Search", "frame", 500, 16, 280, 40,
                    fills=[solid(1, 1, 1, 0.06)], radius=[20, 20, 20, 20],
                    children=[text("S:5", "Hint", 16, 10, 120, 20, "Search items...", 14, "Regular", (1, 1, 1, 0.4))]),
                node("S:6", "btn_Close", "frame", 1216, 16, 40, 40,
                    fills=[solid(1, 1, 1, 0.08)], radius=[20, 20, 20, 20],
                    layout={"mode": "horizontal", "mainAlign": "center", "crossAlign": "center"},
                    states={"hovered": {"fills": [solid(1, 0.3, 0.3, 0.6)], "radius": [20, 20, 20, 20]}},
                    children=[text("S:7", "X", 14, 8, 12, 24, "X", 18, "Bold")]),
            ]),
        # Item grid: tile view of component instances
        node("S:10", "tile_Items", "frame", 32, 104, 560, 584,
            constraints={"h": "min", "v": "stretch"},
            layout={"mode": "wrap", "gap": 16, "crossGap": 16},
            children=[
                node("S:11", "Item1", "instance", 0, 0, 160, 200, component={"ref": "C:1", "name": "ItemCard"}),
                node("S:12", "Item2", "instance", 176, 0, 160, 200, component={"ref": "C:1", "name": "ItemCard"}),
                node("S:13", "Item3", "instance", 352, 0, 160, 200, component={"ref": "C:1", "name": "ItemCard"}),
                node("S:14", "Item4", "instance", 0, 216, 160, 200, component={"ref": "C:1", "name": "ItemCard"}),
            ]),
        # Offer panel: gradient + shadow background, vertical auto layout
        node("P:1", "OfferPanel", "frame", 900, 104, 340, 260,
            constraints={"h": "max", "v": "min"},
            layout={"mode": "vertical", "gap": 12, "padding": [20, 20, 20, 20], "crossAlign": "stretch"},
            fills=[{"type": "linear", "stops": [{"position": 0, "color": [0.14, 0.16, 0.22, 1]}, {"position": 1, "color": [0.09, 0.1, 0.13, 1]}],
                    "handles": [0.5, 0, 0.5, 1, 1, 0]}],
            radius=[16, 16, 16, 16],
            stroke={"color": [1, 1, 1, 0.1], "width": 1, "align": "inside"},
            effects=[{"type": "drop-shadow", "color": [0, 0, 0, 0.5], "offset": [0, 8], "blur": 24, "spread": 0}],
            children=panel_children + [
                node("P:11", "NewBadge", "frame", 290, -10, 56, 24, positioning="absolute",
                    constraints={"h": "max", "v": "min"},
                    fills=[solid(0.95, 0.3, 0.35)], radius=[12, 12, 12, 12],
                    layout={"mode": "horizontal", "mainAlign": "center", "crossAlign": "center"},
                    children=[text("P:12", "BadgeText", 12, 4, 32, 16, "NEW", 11, "Bold")]),
            ]),
        # Group (flattened into the canvas)
        node("S:20", "Decor", "group", 900, 400, 120, 80, children=[
            node("S:21", "DecorA", "rectangle", 0, 0, 80, 80, fills=[solid(0.3, 0.35, 0.5, 0.5)], radius=[40, 40, 40, 40]),
            node("S:22", "DecorB", "rectangle", 40, 0, 80, 80, fills=[solid(0.5, 0.3, 0.45, 0.5)], radius=[40, 40, 40, 40]),
        ]),
        # Baked vector with glow overflow
        node("S:30", "StarIcon", "vector", 1060, 416, 48, 48, bake={"image": "img_star", "overflow": [8, 8, 8, 8]}),
        # Frosted strip
        node("S:40", "Frosted", "frame", 900, 520, 340, 64,
            constraints={"h": "max", "v": "max"},
            fills=[solid(1, 1, 1, 0.05)], radius=[12, 12, 12, 12],
            effects=[{"type": "background-blur", "radius": 20}],
            layout={"mode": "horizontal", "padding": [16, 0, 16, 0], "crossAlign": "center"},
            children=[text("S:41", "$Score", 16, 20, 200, 24, "Gold: 12,400", 18, "SemiBold", (1, 0.85, 0.35, 1))]),
        # Hidden layer stays in the tree, collapsed
        node("S:50", "DebugOverlay", "rectangle", 0, 0, 1280, 720, visible=False, fills=[solid(1, 0, 0, 0.2)]),
        # Ignored layer
        node("S:60", "_notes", "frame", 0, 700, 200, 20),
    ])

doc = {
    "schema": "designbridge/1",
    "source": {"tool": "sample", "file": "sample_shop", "exporter": "make_sample.py"},
    "images": {
        "img_thumb": {"data": b64(png(80, 80, thumb)), "scale": 1},
        "img_star": {"data": b64(png(128, 128, star)), "scale": 2},
    },
    "documents": [{"id": "S:1", "name": "Shop", "kind": "screen", "root": root}],
    "components": [item_card],
    "lint": [{"level": "info", "node": "S:30", "name": "StarIcon", "message": "Vector baked to image (2x)"}],
}

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    json.dump(doc, f, indent=1)
print("wrote", os.path.normpath(OUT))

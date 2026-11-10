# DwoCanvas - writes plugin/DwoCanvas/Samples/sample_settings.dwocanvas.json (dwocanvas/2): a settings screen that exercises the
# mappings sample_shop.json does not (controls, switcher, scroll, grid, gradients, 9-slice, fit / tile images,
# rotation, center / scale constraints, named slot, instance overrides). Two warnings are intentional
# (a missing string table and an unbaked blend mode) so the result window shows its "check" state.
# role / variable / widgetName come from the default naming rules (canvas/python/dwocanvas/naming.json).
#
#   python make_sample_settings.py

import base64
import json
import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "plugin", "DwoCanvas", "Samples", "sample_settings.dwocanvas.json")
sys.path.insert(0, os.path.join(HERE, "..", "python"))

from dwocanvas.migrate import font_weight  # noqa: E402
from dwocanvas.naming import apply_naming  # noqa: E402

ADAPTER = "dwocanvas-sample/0.2.0"


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


def panel_9slice(x, y):
    # 48x48 rounded panel with a 2px light border; corners 12px -> nineSlice 12
    size, r = 48, 12
    cx = min(max(x, r), size - 1 - r)
    cy = min(max(y, r), size - 1 - r)
    d = math.hypot(x - cx, y - cy)
    if d > r:
        return (0, 0, 0, 0)
    if d > r - 2 or x < 2 or y < 2 or x > size - 3 or y > size - 3:
        return (120, 170, 255, 255)
    return (24, 30, 44, 235)


def avatar(x, y):
    # 120x80 landscape picture (fit -> letterboxed in a square)
    sky = (90 + y, 140 + y // 2, 220, 255)
    if y > 55 + 8 * math.sin(x / 12.0):
        return (60, 130 + x // 4, 70, 255)
    if math.hypot(x - 90, y - 22) < 10:
        return (255, 230, 120, 255)
    return sky


def stripe(x, y):
    # 16x16 diagonal stripe tile
    return (255, 200, 60, 255) if (x + y) % 16 < 8 else (40, 40, 48, 255)


def node(id_, name, type_, x, y, w, h, **extra):
    n = {"id": id_, "name": name, "type": type_, "box": {"x": x, "y": y, "w": w, "h": h}}
    n.update(extra)
    return n


def text(id_, name, x, y, w, h, content, size=14, style="Regular", color=(0.92, 0.94, 0.97, 1), auto="width", **extra):
    weight, italic = font_weight(style)
    t = {"content": content, "font": {"family": "Roboto", "weight": weight, "italic": italic, "size": size, "style": style},
         "color": list(color), "autoResize": auto}
    t.update(extra.pop("text", {}))
    return node(id_, name, "text", x, y, w, h, text=t, **extra)


def solid(r, g, b, a=1.0, **extra):
    paint = {"type": "solid", "color": [r, g, b, a]}
    paint.update(extra)
    return paint


ACCENT = (0.36, 0.62, 1.0)
PANEL = (0.09, 0.11, 0.15)
ROW = (0.13, 0.15, 0.2)

# ---------------------------------------------------------------------------------------------------------------------
# Component: one settings row (label left, value right)

setting_row = {
    "id": "K:1",
    "name": "SettingRow",
    "root": node("K:1", "SettingRow", "component", 0, 0, 520, 40,
        layout={"mode": "horizontal", "gap": 12, "padding": [14, 0, 14, 0], "mainAlign": "space-between", "crossAlign": "center"},
        fills=[solid(*ROW)], radius=[8, 8, 8, 8],
        children=[
            text("K:2", "$Label", 14, 11, 200, 18, "Setting", 14),
            text("K:3", "$Value", 440, 11, 60, 18, "Value", 14, "Bold", (*ACCENT, 1)),
        ]),
}


def row(id_, label, value, y):
    # Overridden text -> inlined (InstancePolicy = InlineWhenOverridden)
    return node(id_, "Row_" + label.replace(" ", ""), "instance", 0, y, 520, 40,
        sizing={"h": "fill", "v": "fixed"},
        component={"ref": "K:1", "name": "SettingRow", "overridden": True},
        layout={"mode": "horizontal", "gap": 12, "padding": [14, 0, 14, 0], "mainAlign": "space-between", "crossAlign": "center"},
        fills=[solid(*ROW)], radius=[8, 8, 8, 8],
        children=[
            text(id_ + "a", "$Label", 14, 11, 200, 18, label, 14),
            text(id_ + "b", "$Value", 440, 11, 60, 18, value, 14, "Bold", (*ACCENT, 1)),
        ])


def nav_button(id_, name, label, y, active=False):
    return node(id_, "btn_" + name, "frame", 0, y, 200, 44,
        sizing={"h": "fill", "v": "fixed"},
        layout={"mode": "horizontal", "padding": [16, 0, 16, 0], "crossAlign": "center"},
        fills=[solid(*ACCENT, 0.25 if active else 0.0)], radius=[10, 10, 10, 10],
        states={"hovered": {"fills": [solid(1, 1, 1, 0.08)], "radius": [10, 10, 10, 10]}},
        children=[text(id_ + "t", "Label", 16, 12, 120, 20, label, 15, "Bold" if active else "Regular")])


# ---------------------------------------------------------------------------------------------------------------------
# Pages

graphics_page = node("G:1", "GraphicsPage", "frame", 0, 0, 560, 520,
    layout={"mode": "vertical", "gap": 10, "crossAlign": "stretch"},
    children=[
        row("G:2", "Resolution", "1920 x 1080", 0),
        row("G:3", "Frame Limit", "144", 50),
        node("G:4", "VSyncRow", "frame", 0, 100, 560, 40,
            sizing={"h": "fill", "v": "fixed"},
            layout={"mode": "horizontal", "padding": [14, 0, 14, 0], "mainAlign": "space-between", "crossAlign": "center"},
            fills=[solid(*ROW)], radius=[8, 8, 8, 8],
            children=[
                text("G:5", "Label", 14, 11, 120, 18, "V-Sync", 14),
                node("G:6", "chk_VSync", "frame", 500, 10, 20, 20, fills=[solid(*ACCENT)], radius=[4, 4, 4, 4]),
            ]),
        node("G:7", "BrightnessRow", "frame", 0, 150, 560, 56,
            sizing={"h": "fill", "v": "fixed"},
            layout={"mode": "vertical", "gap": 6, "padding": [14, 8, 14, 8]},
            fills=[solid(*ROW)], radius=[8, 8, 8, 8],
            children=[
                text("G:8", "Label", 14, 8, 120, 16, "Brightness", 13),
                node("G:9", "slider_Brightness", "frame", 14, 30, 532, 16, sizing={"h": "fill", "v": "fixed"}),
            ]),
        text("G:10", "PresetTitle", 0, 220, 200, 18, "Quality Presets", 13, "Bold", (0.7, 0.75, 0.85, 1),
            text={"letterSpacing": {"unit": "px", "value": 1.5}, "case": "upper"}),
        node("G:11", "Presets", "frame", 0, 246, 560, 150,
            sizing={"h": "fill", "v": "hug"},
            layout={"mode": "grid", "columns": 3, "gap": 10, "crossGap": 10},
            children=[
                node("G:12", "Low", "rectangle", 0, 0, 180, 70, sizing={"h": "fill", "v": "fixed"},
                    fills=[{"type": "radial", "stops": [{"position": 0, "color": [0.3, 0.35, 0.45, 1]}, {"position": 1, "color": [0.12, 0.14, 0.18, 1]}],
                            "handles": [0.5, 0.5, 1.0, 0.5, 0.5, 1.0]}], radius=[10, 10, 10, 10]),
                node("G:13", "Medium", "rectangle", 190, 0, 180, 70, sizing={"h": "fill", "v": "fixed"},
                    fills=[{"type": "angular", "stops": [{"position": 0, "color": [0.36, 0.62, 1, 1]}, {"position": 0.5, "color": [0.75, 0.4, 1, 1]},
                            {"position": 1, "color": [0.36, 0.62, 1, 1]}], "handles": [0.5, 0.5, 1.0, 0.5, 0.5, 1.0]}], radius=[10, 10, 10, 10]),
                node("G:14", "High", "rectangle", 380, 0, 180, 70, sizing={"h": "fill", "v": "fixed"},
                    fills=[{"type": "diamond", "stops": [{"position": 0, "color": [1, 0.75, 0.3, 1]}, {"position": 1, "color": [0.55, 0.25, 0.1, 1]}],
                            "handles": [0.5, 0.5, 1.0, 0.5, 0.5, 1.0]}], radius=[10, 10, 10, 10]),
                node("G:15", "Ultra", "rectangle", 0, 80, 180, 70, sizing={"h": "fill", "v": "fixed"},
                    fills=[solid(0.18, 0.2, 0.26)], radius=[10, 10, 10, 10],
                    stroke={"color": [1, 0.8, 0.3, 1], "width": 3, "align": "outside"}),
                node("G:16", "Custom", "rectangle", 190, 80, 180, 70, sizing={"h": "fill", "v": "fixed"},
                    fills=[{"type": "image", "image": "img_stripe", "scaleMode": "tile", "scalingFactor": 1}], radius=[10, 10, 10, 10]),
                node("G:17", "Off", "rectangle", 380, 80, 180, 70, sizing={"h": "fill", "v": "fixed"},
                    fills=[solid(0.2, 0.2, 0.22)], radius=[10, 10, 10, 10], blendMode="multiply"),
            ]),
    ])

audio_page = node("A:1", "AudioPage", "frame", 0, 0, 560, 520,
    layout={"mode": "vertical", "gap": 10, "crossAlign": "stretch"},
    children=[
        text("A:2", "MasterLabel", 0, 0, 200, 18, "Master Volume", 13),
        node("A:3", "progress_MasterVolume", "frame", 0, 26, 560, 12, sizing={"h": "fill", "v": "fixed"},
            fills=[solid(0.2, 0.22, 0.28)], radius=[6, 6, 6, 6]),
        text("A:4", "MusicLabel", 0, 50, 200, 18, "Music", 13),
        node("A:5", "slider_Music", "frame", 0, 76, 560, 16, sizing={"h": "fill", "v": "fixed"}),
        row("A:6", "Output Device", "Speakers", 110),
    ])

key_rows = [row("C:%d" % (10 + i), action, key, i * 48) for i, (action, key) in enumerate([
    ("Move Forward", "W"), ("Move Back", "S"), ("Strafe Left", "A"), ("Strafe Right", "D"), ("Jump", "Space"),
    ("Crouch", "Ctrl"), ("Interact", "E"), ("Inventory", "Tab"), ("Map", "M"), ("Pause", "Esc")])]

controls_page = node("C:1", "ControlsPage", "frame", 0, 0, 560, 520,
    layout={"mode": "vertical", "gap": 8},
    children=[node("C:2", "scroll_KeyBindings", "frame", 0, 0, 560, 520,
        sizing={"h": "fill", "v": "fill"},
        layout={"mode": "vertical", "gap": 8, "crossAlign": "stretch"},
        children=key_rows)])

# ---------------------------------------------------------------------------------------------------------------------
# Screen

root = node("S:1", "Settings", "frame", 0, 0, 1280, 720,
    fills=[{"type": "radial", "stops": [{"position": 0, "color": [0.12, 0.15, 0.22, 1]}, {"position": 1, "color": [0.04, 0.05, 0.07, 1]}],
            "handles": [0.5, 0.35, 1.1, 0.35, 0.5, 1.2]}],
    clip=True,
    children=[
        # Centered dialog
        node("S:2", "Dialog", "frame", 140, 60, 1000, 600,
            constraints={"h": "center", "v": "center"},
            layout={"mode": "horizontal", "gap": 24, "padding": [24, 24, 24, 24]},
            fills=[solid(*PANEL, 0.96)], radius=[18, 18, 18, 18],
            stroke={"color": [1, 1, 1, 0.08], "width": 1, "align": "inside"},
            effects=[{"type": "drop-shadow", "color": [0, 0, 0, 0.55], "offset": [0, 16], "blur": 40, "spread": 0}],
            children=[
                # Sidebar
                node("S:3", "Sidebar", "frame", 24, 24, 200, 552,
                    sizing={"h": "fixed", "v": "fill"},
                    layout={"mode": "vertical", "gap": 6},
                    children=[
                        text("S:4", "Title", 0, 0, 200, 32, "Settings", 26, "Bold"),
                        node("S:5", "spacer_Title", "frame", 0, 32, 10, 18),
                        nav_button("S:6", "TabGraphics", "Graphics", 50, True),
                        nav_button("S:7", "TabAudio", "Audio", 100),
                        nav_button("S:8", "TabControls", "Controls", 150),
                        node("S:9", "spacer_Fill", "frame", 0, 200, 10, 10, sizing={"h": "fixed", "v": "fill"}),
                        # Profile card: fit image + text input
                        node("S:10", "Profile", "frame", 0, 400, 200, 150,
                            layout={"mode": "vertical", "gap": 8, "padding": [10, 10, 10, 10], "crossAlign": "stretch"},
                            fills=[{"type": "image", "image": "img_panel", "scaleMode": "fill"}],
                            children=[
                                node("S:11", "Avatar", "rectangle", 10, 10, 180, 70, sizing={"h": "fill", "v": "fixed"},
                                    fills=[solid(0.05, 0.06, 0.08), {"type": "image", "image": "img_avatar", "scaleMode": "fit"}]),
                                node("S:12", "input_PlayerName", "frame", 10, 88, 180, 32, sizing={"h": "fill", "v": "fixed"},
                                    fills=[solid(1, 1, 1, 0.06)], radius=[6, 6, 6, 6],
                                    children=[text("S:13", "Hint", 8, 7, 120, 18, "Player name", 13, color=(1, 1, 1, 0.4))]),
                            ]),
                    ]),
                # Content: page switcher + footer
                node("S:20", "Content", "frame", 248, 24, 728, 552,
                    sizing={"h": "fill", "v": "fill"},
                    layout={"mode": "vertical", "gap": 16},
                    children=[
                        node("S:21", "switch_Pages", "frame", 0, 0, 728, 480, sizing={"h": "fill", "v": "fill"},
                            children=[graphics_page, audio_page, controls_page]),
                        node("S:22", "Footer", "frame", 0, 496, 728, 56,
                            sizing={"h": "fill", "v": "fixed"},
                            layout={"mode": "horizontal", "gap": 10, "crossAlign": "center"},
                            children=[
                                text("S:23", "$Version", 0, 18, 260, 20, "Build 0.9.4-beta (2026-11-11) - very long build string that gets truncated",
                                    12, color=(0.55, 0.6, 0.7, 1), auto="truncate", sizing={"h": "fill", "v": "hug"}),
                                node("S:24", "slot_Extra", "frame", 280, 10, 120, 36),
                                node("S:25", "btn_Cancel", "frame", 420, 6, 140, 44,
                                    layout={"mode": "horizontal", "mainAlign": "center", "crossAlign": "center"},
                                    fills=[solid(1, 1, 1, 0.08)], radius=[10, 10, 10, 10],
                                    states={"hovered": {"fills": [solid(1, 1, 1, 0.14)], "radius": [10, 10, 10, 10]},
                                            "pressed": {"fills": [solid(1, 1, 1, 0.04)], "radius": [10, 10, 10, 10]}},
                                    children=[text("S:26", "Label", 40, 12, 60, 20, "Cancel", 15)]),
                                node("S:27", "btn_Apply", "frame", 570, 6, 158, 44,
                                    layout={"mode": "horizontal", "mainAlign": "center", "crossAlign": "center"},
                                    fills=[solid(*ACCENT, colorToken="color.accent")], radius=[10, 10, 10, 10],
                                    states={"hovered": {"fills": [solid(0.46, 0.7, 1.0)], "radius": [10, 10, 10, 10]},
                                            "pressed": {"fills": [solid(0.28, 0.52, 0.9)], "radius": [10, 10, 10, 10]},
                                            "disabled": {"fills": [solid(0.25, 0.27, 0.32)], "radius": [10, 10, 10, 10]}},
                                    children=[text("S:28", "Label", 50, 12, 60, 20, "Apply", 15, "Bold",
                                        text={"loc": {"table": "ST_Settings", "key": "Apply"}})]),
                            ]),
                    ]),
                # Rotated badge on the dialog corner
                node("S:30", "BetaBadge", "frame", 930, -12, 84, 28, positioning="absolute", rotation=8,
                    layout={"mode": "horizontal", "mainAlign": "center", "crossAlign": "center"},
                    fills=[solid(1, 0.42, 0.3)], radius=[14, 14, 14, 14],
                    children=[text("S:31", "Text", 22, 6, 40, 16, "BETA", 12, "Bold")]),
            ]),
        # Scaled backdrop strip at the bottom
        node("S:40", "Backdrop", "rectangle", 0, 680, 1280, 40, constraints={"h": "scale", "v": "max"},
            fills=[{"type": "linear", "stops": [{"position": 0, "color": [0.36, 0.62, 1, 0]}, {"position": 0.5, "color": [0.36, 0.62, 1, 0.35]},
                    {"position": 1, "color": [0.36, 0.62, 1, 0]}], "handles": [0, 0.5, 1, 0.5, 0, 1]}]),
    ])

doc = {
    "schema": "dwocanvas/2",
    "source": {"tool": "hand", "adapter": ADAPTER, "mode": "extract", "file": "sample_settings", "x-generator": "canvas/tools/make_sample_settings.py"},
    # No preview render: "preview" is left out
    "capabilities": ["layout.flow", "layout.constraints", "paint.gradient", "paint.image", "effects", "components", "variants", "tokens", "stable-ids"],
    "tokens": {"color": {"accent": {"$type": "color", "$value": "#5c9eff"}}},
    "assets": {
        "img_panel": {"data": b64(png(48, 48, panel_9slice)), "scale": 1, "nineSlice": [12, 12, 12, 12]},
        "img_avatar": {"data": b64(png(120, 80, avatar)), "scale": 1},
        "img_stripe": {"data": b64(png(16, 16, stripe)), "scale": 1},
    },
    "documents": [{"id": "S:1", "name": "Settings", "kind": "screen", "root": root}],
    "components": [setting_row],
    "lint": [{"level": "info", "node": "G:17", "name": "Off", "message": "Blend mode left unbaked on purpose (sample)", "rule": "sample"}],
}
apply_naming(doc)

os.makedirs(os.path.dirname(OUT), exist_ok=True)
with open(OUT, "w", encoding="utf-8", newline="\n") as f:
    json.dump(doc, f, indent=1)
print("wrote", os.path.normpath(OUT))

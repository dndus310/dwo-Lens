"""Tests for the HTML adapter.

    python -m unittest discover -s canvas/adapters/html/tests -v

Mapping tests run on synthetic extractor output (no browser). Tests marked "browser" need Edge or Chrome and are
skipped when neither is installed.
"""

from __future__ import annotations

import copy
import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ADAPTER_DIR = HERE.parent
sys.path.insert(0, str(ADAPTER_DIR))

import html2dwocanvas as h  # noqa: E402
import pngio  # noqa: E402
from dwocanvas import validate as validate_module  # noqa: E402

REPO = ADAPTER_DIR.parents[2]
CONFORMANCE = REPO / "plugin" / "DwoCanvas" / "Conformance"
HAS_BROWSER = bool(h.find_browsers())


# ---------------------------------------------------------------------------------------------------------------------
# Synthetic extractor output

BASE_STYLE = {
    "display": "block", "position": "static", "visibility": "visible", "opacity": "1", "overflowX": "visible", "overflowY": "visible",
    "flexDirection": "row", "flexWrap": "nowrap", "justifyContent": "normal", "alignItems": "normal", "alignSelf": "auto",
    "flexGrow": "0", "rowGap": "normal", "columnGap": "normal", "gridTemplateColumns": "none", "gridAutoFlow": "row",
    "paddingLeft": "0px", "paddingTop": "0px", "paddingRight": "0px", "paddingBottom": "0px",
    "marginLeft": "0px", "marginTop": "0px", "marginRight": "0px", "marginBottom": "0px",
    **{f"border{s}Width": "0px" for s in ("Left", "Top", "Right", "Bottom")},
    **{f"border{s}Style": "none" for s in ("Left", "Top", "Right", "Bottom")},
    **{f"border{s}Color": "rgb(0, 0, 0)" for s in ("Left", "Top", "Right", "Bottom")},
    **{k: "0px" for k in ("borderTopLeftRadius", "borderTopRightRadius", "borderBottomRightRadius", "borderBottomLeftRadius")},
    "backgroundColor": "rgba(0, 0, 0, 0)", "backgroundImage": "none", "backgroundSize": "auto", "backgroundRepeat": "repeat",
    "backgroundClip": "border-box", "boxShadow": "none", "backdropFilter": "none", "filter": "none", "mixBlendMode": "normal",
    "color": "rgb(255, 255, 255)", "fontFamily": "Roboto", "fontWeight": "400", "fontStyle": "normal", "fontSize": "13px",
    "lineHeight": "15px", "letterSpacing": "normal", "textAlign": "start", "textTransform": "none", "textOverflow": "clip",
    "whiteSpace": "normal", "webkitLineClamp": "none", "textDecorationLine": "none", "textShadow": "none",
    "objectFit": "fill", "objectPosition": "50% 50%",
}


def el(tag="div", rect=(0, 0, 10, 10), attrs=None, children=None, text=None, sizing=None, **style):
    node = {"kind": "element", "tag": tag, "attrs": attrs or {}, "path": f"body>{tag}:{id(style)}",
            "rect": dict(zip("xywh", rect)), "style": {**BASE_STYLE, **style}}
    if children is not None:
        node["children"] = children
    if text is not None:
        node["text"] = text
    if sizing:
        node["sizing"] = {"h": sizing[0], "v": sizing[1]}
    return node


def text_info(content, rect, lines=1, content_height=15):
    return {"content": content, "rect": dict(zip("xywh", rect)), "lines": lines, "contentHeight": content_height, "mixed": False}


def raw_doc(body, tokens=None, html_bg="rgba(0, 0, 0, 0)"):
    return {"viewport": {"w": 400, "h": 300}, "canvas": {"html": html_bg}, "fonts": [{"family": "Roboto", "status": "loaded"}],
            "tokens": tokens or {}, "warnings": [], "body": body}


def convert(body, **kw):
    return h.convert_raw(raw_doc(body, **kw), "page.html", 400, 300, "Page", None)


def root_of(doc):
    return doc["documents"][0]["root"]


def errors(doc):
    return [i for i in validate_module.validate(doc) if i["level"] == "error"]


# ---------------------------------------------------------------------------------------------------------------------

class CssParsingTest(unittest.TestCase):
    def test_colors(self):
        self.assertEqual(h.parse_color("rgb(255, 0, 51)"), [1.0, 0.0, 0.2, 1.0])
        self.assertEqual(h.parse_color("rgba(0, 0, 0, 0.35)"), [0.0, 0.0, 0.0, 0.35])
        self.assertEqual(h.parse_color("rgb(255 255 255 / 0.5)"), [1.0, 1.0, 1.0, 0.5])
        self.assertEqual(h.parse_color("#336699"), [0.2, 0.4, 0.6, 1.0])
        self.assertEqual(h.parse_color("#fff8"), [1.0, 1.0, 1.0, 0.5333])
        self.assertIsNone(h.parse_color("transparent"))
        self.assertIsNone(h.parse_color("none"))

    def test_split_top(self):
        self.assertEqual(h.split_top("linear-gradient(red, blue), url(a,b.png)"), ["linear-gradient(red, blue)", "url(a,b.png)"])
        self.assertEqual(h.split_top("rgba(0, 0, 0, 0.5) 0px 2px", " "), ["rgba(0, 0, 0, 0.5)", "0px", "2px"])

    def test_shadows(self):
        shadows = h.parse_shadows("rgba(0, 0, 0, 0.35) 0px 6px 18px 0px, rgb(255, 255, 255) 0px 1px 0px 0px inset")
        self.assertEqual(len(shadows), 2)
        self.assertEqual(shadows[0], {"inset": False, "color": [0.0, 0.0, 0.0, 0.35], "offset": [0.0, 6.0], "blur": 18.0, "spread": 0.0})
        self.assertTrue(shadows[1]["inset"])
        self.assertEqual(h.parse_shadows("none"), [])

    def test_linear_gradient_handles(self):
        paint, _ = h.gradient_paint("linear-gradient(to right, rgb(255, 0, 0), rgb(0, 0, 255))", 200, 100)
        self.assertEqual(paint["handles"][:4], [0.0, 0.5, 1.0, 0.5])
        self.assertEqual([s["position"] for s in paint["stops"]], [0.0, 1.0])
        paint, _ = h.gradient_paint("linear-gradient(rgb(255, 0, 0), rgb(0, 255, 0) 25%, rgb(0, 0, 255))", 100, 100)
        self.assertEqual(paint["handles"][:4], [0.5, 0.0, 0.5, 1.0])  # default: to bottom
        self.assertEqual([s["position"] for s in paint["stops"]], [0.0, 0.25, 1.0])
        # 135deg on a square: corner to corner
        paint, _ = h.gradient_paint("linear-gradient(135deg, rgb(255, 0, 0) 0%, rgb(0, 0, 255) 100%)", 100, 100)
        self.assertEqual(paint["handles"][:4], [0.0, 0.0, 1.0, 1.0])

    def test_linear_gradient_corner_keyword(self):
        # 'to top right' on 200x100: the 50% line runs through the top-left and bottom-right corners
        paint, _ = h.gradient_paint("linear-gradient(to top right, red, blue)", 200, 100)
        sx, sy, ex, ey = paint["handles"][:4]
        dx, dy = (ex - sx) * 200, (ey - sy) * 100
        self.assertAlmostEqual(dx * 200 + dy * 100, 0.0, delta=1.0)  # perpendicular to the (200, 100) diagonal
        self.assertGreater(dx, 0)
        self.assertLess(dy, 0)

    def test_radial_gradient(self):
        paint, _ = h.gradient_paint("radial-gradient(circle at 50% 50%, rgb(255, 255, 255) 0%, rgb(0, 0, 0) 100%)", 100, 100)
        self.assertEqual(paint["type"], "radial")
        cx, cy, ex, ey, wx, wy = paint["handles"]
        self.assertEqual((cx, cy), (0.5, 0.5))
        self.assertAlmostEqual((ex - cx) * 100, math.hypot(50, 50), places=1)  # circle farthest-corner
        paint, _ = h.gradient_paint("radial-gradient(closest-side, red, blue)", 200, 100)
        self.assertEqual(paint["handles"], [0.5, 0.5, 1.0, 0.5, 0.5, 1.0])  # ellipse 100 x 50

    def test_style_names(self):
        self.assertEqual(h.style_name(700, False), "Bold")
        self.assertEqual(h.style_name(400, True), "Italic")
        self.assertEqual(h.style_name(700, True), "Bold Italic")
        self.assertEqual(h.style_name(550, False), "Medium")

    def test_inject_and_meta(self):
        page = '<html><head><meta name="dwocanvas:viewport" content="400x300"><meta name="dwocanvas:name" content="Shop"></head><body>x</body></html>'
        out = h.inject(page, "file:///c:/pages/", "var a = 1;")
        self.assertIn('<head><meta charset="utf-8"><base href="file:///c:/pages/">', out)
        self.assertTrue(out.endswith("<script>\nvar a = 1;\n</script></body></html>"))
        self.assertEqual(h.page_meta(page), {"viewport": "400x300", "name": "Shop"})


class PngTest(unittest.TestCase):
    def test_roundtrip_and_crop(self):
        rgba = bytearray()
        for y in range(5):
            for x in range(7):
                rgba += bytes((x * 30, y * 50, 7, 200))
        png = pngio.encode_png(7, 5, rgba)
        self.assertEqual(pngio.image_size(png), (7, 5))
        w, hgt, back = pngio.decode_png(png)
        self.assertEqual((w, hgt, bytes(back)), (7, 5, bytes(rgba)))
        crop = pngio.crop_rgba(7, 5, rgba, 5, 3, 4, 3)  # partly outside: transparent there
        self.assertEqual(crop[0:4], bytes((150, 150, 7, 200)))
        self.assertEqual(crop[8:12], bytes(4))

    def test_jpeg_size(self):
        # SOI, APP0 (len 4), SOF0 (h=20, w=30)
        jpeg = b"\xff\xd8" + b"\xff\xe0\x00\x04\x00\x00" + b"\xff\xc0\x00\x11\x08\x00\x14\x00\x1e" + b"\x00" * 12
        self.assertEqual(pngio.image_size(jpeg), (30, 20))


class MappingTest(unittest.TestCase):
    def flex_row(self):
        return el("body", (0, 0, 400, 300), {"data-db-id": "page"}, children=[
            el("div", (20, 20, 200, 60), {"id": "row", "data-name": "Toolbar"}, display="flex", columnGap="8px", rowGap="8px",
               paddingLeft="10px", paddingTop="10px", paddingRight="10px", paddingBottom="10px", alignItems="center",
               justifyContent="space-between", backgroundColor="rgb(51, 102, 153)",
               borderTopLeftRadius="4px", borderTopRightRadius="4px", borderBottomRightRadius="0px", borderBottomLeftRadius="12px",
               boxShadow="rgba(0, 0, 0, 0.5) 0px 2px 4px 0px",
               children=[
                   el("div", (30, 30, 40, 40), {"id": "btn_Ok"}, sizing=("fixed", "fixed"), backgroundColor="rgb(255, 0, 0)"),
                   el("span", (78, 42, 50, 15), {"id": "label"}, sizing=("hug", "hug"), text=text_info("Hello", (78, 42, 50, 15))),
                   el("div", (136, 30, 74, 40), {"id": "grow"}, sizing=("fill", "fixed"), flexGrow="1"),
               ]),
        ])

    def test_flex_row(self):
        doc = convert(self.flex_row())
        self.assertEqual(errors(doc), [])
        root = root_of(doc)
        row = root["children"][0]  # plain block body: flattened into the root
        self.assertEqual(row["box"], {"x": 20.0, "y": 20.0, "w": 200.0, "h": 60.0})
        self.assertEqual(row["layout"], {"mode": "horizontal", "gap": 8.0, "padding": [10.0, 10.0, 10.0, 10.0],
                                         "mainAlign": "space-between", "crossAlign": "center"})
        self.assertEqual(row["radius"], [4.0, 4.0, 0.0, 12.0])
        self.assertEqual(row["effects"][0]["type"], "drop-shadow")
        self.assertEqual(row["fills"], [{"type": "solid", "color": [0.2, 0.4, 0.6, 1.0]}])
        ok, label, grow = row["children"]
        self.assertEqual(ok["box"]["x"], 10.0)  # relative to the parent
        self.assertEqual((ok["role"], ok["variable"], ok["widgetName"]), ("button", True, "Ok"))  # naming rules
        self.assertEqual(label["type"], "text")
        self.assertEqual(label["text"]["autoResize"], "width")
        self.assertEqual(label["text"]["font"], {"family": "Roboto", "weight": 400, "italic": False, "size": 13.0, "style": "Regular"})
        self.assertEqual(grow["sizing"], {"h": "fill", "v": "fixed"})
        self.assertIn("stable-ids", doc["capabilities"])
        self.assertEqual(doc["source"], {"tool": "html", "adapter": h.ADAPTER, "mode": "extract", "file": "page.html"})

    def test_stretch_kept_only_when_all_children_fill(self):
        body = self.flex_row()
        row = body["children"][0]
        row["style"]["alignItems"] = "normal"
        for child in row["children"]:
            child["sizing"] = {"h": child["sizing"]["h"], "v": "fill"}
        self.assertEqual(root_of(convert(copy.deepcopy(body)))["children"][0]["layout"]["crossAlign"], "stretch")
        row["children"][0]["sizing"]["v"] = "fixed"
        self.assertEqual(root_of(convert(body))["children"][0]["layout"]["crossAlign"], "start")

    def test_hashed_ids_without_stable_ids(self):
        body = el("body", (0, 0, 400, 300), children=[el("div", (0, 0, 10, 10), backgroundColor="rgb(0, 0, 0)")])
        doc = convert(body)
        self.assertNotIn("stable-ids", doc["capabilities"])
        child = root_of(doc)["children"][0]
        self.assertRegex(child["id"], r"^h[0-9a-f]{10}$")
        self.assertTrue(any(i["rule"] == "stable-ids" and i["level"] == "info" for i in doc["lint"]))

    def test_data_attributes_win(self):
        body = el("body", (0, 0, 400, 300), children=[
            el("div", (0, 0, 10, 10), {"data-db-id": "x1", "id": "btn_Other", "data-name": "btn_Play", "data-role": "x-shop.card",
                                       "data-variable": "false", "data-widget-name": "PlayCard"}, backgroundColor="rgb(0, 0, 0)")])
        child = root_of(convert(body))["children"][0]
        self.assertEqual((child["id"], child["name"], child["role"], child["variable"], child["widgetName"]),
                         ("x1", "btn_Play", "x-shop.card", False, "PlayCard"))

    def test_text_with_visuals_becomes_frame_with_label(self):
        body = el("body", (0, 0, 400, 300), children=[
            el("div", (10, 10, 100, 32), {"id": "pill"}, text=text_info("Go", (48, 18.5, 24, 15)), backgroundColor="rgb(0, 128, 255)",
               paddingLeft="12px", paddingRight="12px", lineHeight="32px", textAlign="center", textTransform="uppercase",
               letterSpacing="1.5px", fontWeight="700")])
        pill = root_of(convert(body))["children"][0]
        self.assertEqual(pill["type"], "frame")
        self.assertEqual(pill["layout"]["mainAlign"], "center")
        label = pill["children"][0]
        self.assertEqual((label["type"], label["name"], label["id"]), ("text", "Label", "pill#text"))
        self.assertEqual(label["box"]["y"], 0.0)  # line box top = text top - half leading ((32 - 15) / 2)
        self.assertEqual(label["box"]["h"], 32.0)
        t = label["text"]
        self.assertEqual((t["case"], t["letterSpacing"], t["lineHeight"], t["font"]["style"]),
                         ("upper", {"unit": "px", "value": 1.5}, {"unit": "px", "value": 32.0}, "Bold"))

    def test_text_truncation_and_clamp(self):
        body = el("body", (0, 0, 400, 300), children=[
            el("div", (0, 0, 100, 15), {"id": "a"}, text=text_info("long", (0, 0, 100, 15)), textOverflow="ellipsis", overflowX="hidden",
               whiteSpace="nowrap"),
            el("div", (0, 20, 100, 30), {"id": "b"}, text=text_info("two lines", (0, 20, 100, 30), lines=2), webkitLineClamp="2")])
        a, b = root_of(convert(body))["children"]
        self.assertEqual(a["text"]["autoResize"], "truncate")
        self.assertEqual((b["text"]["autoResize"], b["text"]["maxLines"]), ("height", 2))

    def test_grid_and_absolute(self):
        cells = [el("div", (10 + i % 2 * 60, 10 + i // 2 * 60, 50, 50), {"id": f"c{i}"}, backgroundColor="rgb(9, 9, 9)") for i in range(4)]
        cells.append(el("div", (0, 0, 8, 8), {"id": "abs"}, position="absolute", backgroundColor="rgb(255, 0, 0)"))
        body = el("body", (0, 0, 400, 300), children=[
            el("div", (0, 0, 130, 130), {"id": "grid"}, display="grid", gridTemplateColumns="50px 50px", columnGap="10px", rowGap="10px",
               paddingLeft="10px", paddingTop="10px", paddingRight="10px", paddingBottom="10px", children=cells)])
        grid = root_of(convert(body))["children"][0]
        self.assertEqual(grid["layout"]["mode"], "grid")
        self.assertEqual((grid["layout"]["columns"], grid["layout"]["gap"], grid["layout"]["crossGap"]), (2, 10.0, 10.0))
        self.assertEqual(grid["children"][-1]["positioning"], "absolute")
        self.assertNotIn("positioning", grid["children"][0])

    def test_body_fills_viewport_becomes_root_layout(self):
        body = el("body", (0, 0, 400, 300), display="flex", flexDirection="column", rowGap="4px", backgroundColor="rgb(10, 20, 30)",
                  children=[el("div", (0, 0, 400, 20), {"id": "a"}, sizing=("fill", "fixed"), backgroundColor="rgb(1, 1, 1)")])
        doc = convert(body)
        root = root_of(doc)
        self.assertEqual(root["layout"]["mode"], "vertical")
        self.assertEqual(root["fills"][0]["color"], [0.0392, 0.0784, 0.1176, 1.0])  # body background = canvas
        self.assertEqual(root["children"][0]["id"], "a")
        self.assertIn("stable-ids", doc["capabilities"])  # the dropped body does not count

    def test_color_tokens(self):
        body = el("body", (0, 0, 400, 300), children=[
            el("div", (0, 0, 10, 10), {"id": "a"}, backgroundColor="rgb(89, 196, 255)")])
        body["children"][0]["tokens"] = {"background": "--accent"}
        doc = convert(body, tokens={"--accent": "#59c4ff"})
        paint = root_of(doc)["children"][0]["fills"][0]
        self.assertEqual(paint["colorToken"], "color.accent")
        self.assertEqual(doc["tokens"], {"color": {"accent": {"$value": "#59c4ff", "$type": "color"}}})
        self.assertIn("tokens", doc["capabilities"])
        self.assertEqual(errors(doc), [])

    def test_rotation_box_is_unrotated(self):
        body = el("body", (0, 0, 400, 300), children=[el("div", (100, 100, 60, 20), {"id": "r"}, backgroundColor="rgb(0, 0, 0)")])
        c, s = math.cos(math.radians(30)), math.sin(math.radians(30))
        body["children"][0]["transform"] = {"matrix": f"matrix({c}, {s}, {-s}, {c}, 0, 0)", "origin": "30px 10px", "cx": 140, "cy": 120}
        node = root_of(convert(body))["children"][0]
        self.assertEqual(node["rotation"], -30.0)  # CSS clockwise -> schema counter-clockwise
        self.assertEqual(node["box"], {"x": 110.0, "y": 110.0, "w": 60.0, "h": 20.0})


@unittest.skipUnless(HAS_BROWSER, "needs Edge or Chrome")
class BrowserTest(unittest.TestCase):
    PAGE = """<!doctype html><html><head><meta charset="utf-8"><style>
html, body { margin: 0; } * { box-sizing: border-box; }
body { background: #202830; font-family: sans-serif; }
#bar { position: absolute; left: 10px; top: 10px; width: 180px; height: 40px; display: flex; gap: 6px; padding: 4px;
       align-items: center; background: linear-gradient(to right, #336699, #6699cc); border-radius: 6px; border: 2px solid #ffffff; }
.sq { width: 20px; height: 20px; background: #ff0000; flex: none; }
#grow { flex: 1; height: 20px; background: rgba(0, 255, 0, 0.5); }
#t { position: absolute; left: 10px; top: 60px; color: #ffffff; font-size: 14px; line-height: 20px; text-transform: uppercase; }
</style></head><body>
<div id="bar"><div class="sq" id="btn_A"></div><div class="sq" id="b"></div><div id="grow"></div></div>
<div id="t">Hello</div>
</body></html>"""

    def test_extract_and_convert(self):
        with tempfile.TemporaryDirectory() as tmp:
            page = Path(tmp) / "page.html"
            page.write_text(self.PAGE, encoding="utf-8")
            out = Path(tmp) / "page.dwocanvas.json"
            code = h.main([str(page), "-o", str(out), "--width", "200", "--height", "100", "--name", "Probe"])
            self.assertEqual(code, 0)
            doc = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(errors(doc), [])
        document = doc["documents"][0]
        self.assertEqual(document["name"], "Probe")
        self.assertEqual(pngio.image_size(__import__("base64").b64decode(doc["assets"][document["preview"]]["data"])), (200, 100))
        self.assertIn("preview", doc["capabilities"])
        bar, text = document["root"]["children"]
        self.assertEqual(bar["box"], {"x": 10.0, "y": 10.0, "w": 180.0, "h": 40.0})
        self.assertEqual(bar["layout"]["mode"], "horizontal")
        self.assertEqual((bar["layout"]["gap"], bar["layout"]["crossAlign"], bar["layout"]["strokesIncluded"]), (6.0, "center", True))
        self.assertEqual(bar["fills"][0]["type"], "linear")
        self.assertEqual(bar["stroke"], {"color": [1.0, 1.0, 1.0, 1.0], "width": 2.0, "align": "inside"})
        a, b, grow = bar["children"]
        self.assertEqual((a["role"], a["sizing"]), ("button", {"h": "fixed", "v": "fixed"}))
        self.assertEqual(grow["sizing"]["h"], "fill")
        self.assertEqual(grow["fills"][0]["color"], [0.0, 1.0, 0.0, 0.5])
        self.assertEqual((text["type"], text["text"]["content"], text["text"]["case"]), ("text", "Hello", "upper"))
        self.assertEqual(text["text"]["autoResize"], "width")
        self.assertIn("stable-ids", doc["capabilities"])  # every element has an id (the plain body is flattened away)


class ConformanceFilesTest(unittest.TestCase):
    def test_cases_validate(self):
        files = sorted(CONFORMANCE.glob("html_*/*.dwocanvas.json"))
        if not files:
            self.skipTest("no HTML conformance cases")
        for path in files:
            with self.subTest(case=path.parent.name):
                doc = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(errors(doc), [])
                self.assertEqual(path.name, path.parent.name + ".dwocanvas.json")
                self.assertTrue((path.parent / (path.parent.name + ".html")).is_file())
                extension = doc.get("x-conformance")
                self.assertIsInstance(extension, dict)
                self.assertLessEqual(extension.get("maxDiff", 0.05), 0.08)
                document = doc["documents"][0]
                self.assertIn(document.get("preview"), doc["assets"])
                self.assertEqual(doc["source"]["tool"], "html")


if __name__ == "__main__":
    unittest.main()

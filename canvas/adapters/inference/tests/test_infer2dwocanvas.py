"""Tests for the inference adapter. No network, no ``anthropic`` package needed: everything runs on MockClient.

Run from the repo root:  python -m unittest discover -s canvas/adapters/inference/tests -v
"""

from __future__ import annotations

import base64
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

HERE = Path(__file__).resolve().parent
ADAPTER_DIR = HERE.parent
sys.path.insert(0, str(ADAPTER_DIR))

import infer2dwocanvas as infer  # noqa: E402
import pngtools  # noqa: E402
from dwocanvas.validate import has_errors, validate  # noqa: E402

SAMPLES = ADAPTER_DIR / "samples"


def node(id_, name, type_="frame", **extra):
    n = {"id": id_, "name": name, "type": type_, "box": {"x": 0, "y": 0, "w": 100, "h": 40}}
    n.update(extra)
    return n


def reply_doc(children=None, **root_extra):
    """A minimal model reply (what the model is asked to write: no source / capabilities / assets)."""
    root = node("root", "Screen", fills=[{"type": "solid", "color": [0.1, 0.1, 0.1, 1]}], confidence=0.9, inferred=[],
                layout={"mode": "vertical", "gap": 8, "padding": [16, 16, 16, 16]},
                children=children if children is not None else [
                    node("title", "Title", "text", text={"content": "Settings", "font": {"family": "Roboto", "weight": 700, "size": 20},
                                                         "color": [1, 1, 1, 1]}, confidence=0.8, inferred=["text.font"]),
                    node("apply", "ApplyButton", role="button", variable=True, widgetName="ApplyButton",
                         fills=[{"type": "solid", "color": [0.2, 0.4, 0.99, 1]}]),  # no confidence: defaulted
                ], **root_extra)
    return {"schema": "dwocanvas/2", "documents": [{"id": "screen", "name": "Whatever", "root": root}]}


def tiny_png(width=32, height=16, rgba=(40, 80, 200, 255)) -> bytes:
    rows = [bytearray(bytes(rgba) * width) for _ in range(height)]
    for x in range(4, 8):  # a small orange patch
        rows[2][x * 4:x * 4 + 4] = bytes((255, 170, 60, 255))
    return pngtools.encode_png(pngtools.Image(width, height, rows))


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.cache = self.dir / "cache"
        self.out = self.dir / "out.dwocanvas.json"

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, data) -> str:
        path = self.dir / name
        if isinstance(data, bytes):
            path.write_bytes(data)
        else:
            path.write_text(json.dumps(data), encoding="utf-8")
        return str(path)

    def run_cli(self, argv, replies=None, client=None):
        """(exit code, stdout, stderr, client). ``replies`` -> a mock file + MockClient recording the calls."""
        argv = list(argv) + ["-o", str(self.out), "--cache-dir", str(self.cache)]
        if replies is not None:
            mock_path = self.write("mock.json", replies)
            argv += ["--mock", mock_path]
            if client is None:
                client = infer.MockClient.from_file(mock_path)
        stdout, stderr = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = infer.main(argv, client=client)
        return code, stdout.getvalue(), stderr.getvalue(), client

    def output(self) -> dict:
        return json.loads(self.out.read_text(encoding="utf-8"))


class HappyPath(Base):
    def test_text_to_valid_draft(self):
        code, stdout, _, client = self.run_cli(["--text", "A settings screen with a title and an Apply button", "--name", "Settings"],
                                               [{"json": reply_doc()}])
        self.assertEqual(code, 0, stdout)
        self.assertEqual(len(client.calls), 1)
        doc = self.output()
        self.assertFalse(has_errors(validate(doc)))
        self.assertEqual(doc["schema"], "dwocanvas/2")
        self.assertEqual(doc["source"]["tool"], "inference")
        self.assertEqual(doc["source"]["mode"], "draft")
        self.assertEqual(doc["source"]["adapter"], "dwocanvas-inference/0.1.0")
        self.assertEqual(doc["source"]["model"], f"claude-opus-5-5 (mock); prompt v{infer.PROMPT_VERSION}")
        self.assertIn("inferred", doc["capabilities"])
        self.assertIn("layout.flow", doc["capabilities"])
        self.assertNotIn("preview", doc["capabilities"])
        self.assertNotIn("tokens", doc["capabilities"])
        document = doc["documents"][0]
        self.assertEqual(document["name"], "Settings")
        self.assertNotIn("preview", document)
        self.assertEqual(document["root"]["box"]["w"], 100)  # text input: the model's box is kept
        apply = document["root"]["children"][1]
        self.assertEqual(apply["confidence"], 0.5)
        self.assertIn("confidence", apply["inferred"])
        self.assertEqual(document["root"]["children"][0]["confidence"], 0.8)
        self.assertIn("lint", doc)
        self.assertIn("Summary: 0 error(s)", stdout)

    def test_request_shape(self):
        _, _, _, client = self.run_cli(["--text", "x"], [{"json": reply_doc()}])
        request = client.calls[0]
        self.assertEqual(request["model"], "claude-opus-5-5")
        self.assertEqual(request["max_tokens"], 64000)
        self.assertEqual(request["output_config"], {"effort": "high"})
        self.assertEqual(request["betas"], ["server-side-fallback-2026-07-01"])
        self.assertEqual(request["fallbacks"], "default")
        for banned in ("temperature", "top_p", "top_k", "thinking"):
            self.assertNotIn(banned, request)
        self.assertNotIn("format", request["output_config"])
        self.assertIn('"dwocanvas/2"', request["system"][0]["text"])  # the schema is in the system prompt
        self.assertIn("confidence", request["system"][0]["text"])
        content = request["messages"][0]["content"]
        self.assertEqual([b["type"] for b in content], ["text"])  # no image for --text

    def test_no_fallback_model_effort(self):
        _, _, _, client = self.run_cli(["--text", "x", "--no-fallback", "--model", "claude-sonnet-5-5", "--effort", "low"],
                                       [{"json": reply_doc()}])
        request = client.calls[0]
        self.assertNotIn("betas", request)
        self.assertNotIn("fallbacks", request)
        self.assertEqual(request["model"], "claude-sonnet-5-5")
        self.assertEqual(request["output_config"], {"effort": "low"})

    def test_ids_made_unique_and_fence_stripped(self):
        doc = reply_doc(children=[node("dup", "A", confidence=1, inferred=[]), node("dup", "B", confidence=1, inferred=[]),
                                  node("", "C", confidence=1, inferred=[])])
        text = "Here is the document:\n```json\n" + json.dumps(doc) + "\n```\n"
        code, stdout, _, _ = self.run_cli(["--text", "x"], [text])
        self.assertEqual(code, 0, stdout)
        ids = [c["id"] for c in self.output()["documents"][0]["root"]["children"]]
        self.assertEqual(len(set(ids)), 3)
        self.assertEqual(ids[0], "dup")


class RetryLoop(Base):
    def test_invalid_json_then_valid(self):
        code, stdout, _, client = self.run_cli(["--text", "x"], ["{ this is not json", {"json": reply_doc()}])
        self.assertEqual(code, 0, stdout)
        self.assertEqual(len(client.calls), 2)
        messages = client.calls[1]["messages"]
        self.assertEqual([m["role"] for m in messages], ["user", "assistant", "user"])
        self.assertEqual(messages[1]["content"][0].text, "{ this is not json")
        self.assertIn("could not be parsed as JSON", messages[2]["content"][0]["text"])
        self.assertEqual(self.output()["source"]["x-inference"]["attempts"], 2)

    def test_schema_error_then_valid(self):
        bad = reply_doc()
        bad["documents"][0]["root"]["children"][1]["role"] = "dropdown"  # not a schema role
        code, stdout, _, client = self.run_cli(["--text", "x"], [{"json": bad}, {"json": reply_doc()}])
        self.assertEqual(code, 0, stdout)
        self.assertEqual(len(client.calls), 2)
        feedback = client.calls[1]["messages"][2]["content"][0]["text"]
        self.assertIn("does not pass the DwoCanvas validator", feedback)
        self.assertIn("[schema] apply 'ApplyButton'", feedback)
        self.assertIn("/role", feedback)

    def test_gives_up_after_max_retries(self):
        bad = reply_doc()
        bad["documents"][0]["root"]["children"][1]["role"] = "dropdown"
        code, _, stderr, client = self.run_cli(["--text", "x", "--max-retries", "1"], [{"json": bad}, {"json": bad}, {"json": reply_doc()}])
        self.assertEqual(code, infer.EXIT_INVALID)
        self.assertEqual(len(client.calls), 2)
        self.assertTrue(self.out.exists())  # written with its lint so the errors can be fixed by hand
        self.assertIn("still has validator errors", stderr)
        self.assertFalse((self.cache).exists() and any(self.cache.iterdir()))  # invalid results are not cached

    def test_never_json(self):
        code, _, stderr, client = self.run_cli(["--text", "x", "--max-retries", "2"], ["nope", "nope", "nope"])
        self.assertEqual(code, infer.EXIT_INVALID)
        self.assertEqual(len(client.calls), 3)
        self.assertIn("No usable document after 3 attempt(s)", stderr)

    def test_truncated_then_valid(self):
        code, stdout, _, client = self.run_cli(["--text", "x"], [{"text": "{\"schema\": ", "stop_reason": "max_tokens"},
                                                                  {"json": reply_doc()}])
        self.assertEqual(code, 0, stdout)
        self.assertEqual(len(client.calls), 2)
        last = client.calls[1]["messages"][-1]
        self.assertEqual(last["role"], "user")
        self.assertIn("cut off at the output limit", last["content"][-1]["text"])

    def test_truncated_every_time(self):
        code, _, stderr, _ = self.run_cli(["--text", "x", "--max-retries", "0"], [{"text": "{", "stop_reason": "max_tokens"}])
        self.assertEqual(code, infer.EXIT_TRUNCATED)
        self.assertIn("max_tokens", stderr)


class ImageInput(Base):
    def test_png_preview_and_hints(self):
        png = tiny_png()
        image = self.write("shot.png", png)
        code, stdout, _, client = self.run_cli(["--image", image, "--name", "Shot"], [{"json": reply_doc()}])
        self.assertEqual(code, 0, stdout)
        doc = self.output()
        self.assertIn("preview", doc["capabilities"])
        self.assertIn("inferred", doc["capabilities"])
        document = doc["documents"][0]
        asset = doc["assets"][document["preview"]]
        self.assertEqual(base64.b64decode(asset["data"]), png)
        self.assertEqual(asset["scale"], 1)
        self.assertEqual(len(asset["sha256"]), 64)
        self.assertEqual(document["root"]["box"], {"x": 0, "y": 0, "w": 32, "h": 16})  # canvas = image size
        self.assertEqual(doc["source"]["file"], "shot.png")
        content = client.calls[0]["messages"][0]["content"]
        self.assertEqual(content[0]["type"], "image")  # image block before the text
        self.assertEqual(content[0]["source"]["media_type"], "image/png")
        self.assertEqual(base64.b64decode(content[0]["source"]["data"]), png)
        self.assertIn("Dominant colors", content[1]["text"])
        self.assertIn("#2850c8", content[1]["text"])
        self.assertIn("Image size: 32 x 16", content[1]["text"])

    def test_crop_bake(self):
        image = self.write("shot.png", tiny_png())
        children = [node("icon", "Icon", "vector", box={"x": 4, "y": 2, "w": 4, "h": 1}, bake={"image": "crop"}, confidence=0.4, inferred=[])]
        code, stdout, _, _ = self.run_cli(["--image", image], [{"json": reply_doc(children=children)}])
        self.assertEqual(code, 0, stdout)
        doc = self.output()
        icon = doc["documents"][0]["root"]["children"][0]
        crop = pngtools.decode_png(base64.b64decode(doc["assets"][icon["bake"]["image"]]["data"]))
        self.assertEqual((crop.width, crop.height), (4, 1))
        self.assertEqual(crop.pixel(0, 0), (255, 170, 60, 255))
        self.assertIn("inference-review", stdout)  # confidence 0.4 is listed for review

    def test_text_input_drops_bake(self):
        children = [node("icon", "Icon", "vector", bake={"image": "crop"}, confidence=0.4, inferred=[])]
        code, _, _, _ = self.run_cli(["--text", "x"], [{"json": reply_doc(children=children)}])
        self.assertEqual(code, 0)
        self.assertNotIn("bake", self.output()["documents"][0]["root"]["children"][0])

    def test_jpeg_skips_hints(self):
        # SOI, APP0 stub, SOF0 (height 20, width 40), EOI: enough for the sniffer and the size reader
        jpeg = (b"\xff\xd8" + b"\xff\xe0\x00\x04ab" + b"\xff\xc0\x00\x0b\x08\x00\x14\x00\x28\x01\x01\x11\x00" + b"\xff\xd9")
        image = self.write("shot.jpg", jpeg)
        code, stdout, _, client = self.run_cli(["--image", image], [{"json": reply_doc()}])
        self.assertEqual(code, 0, stdout)
        content = client.calls[0]["messages"][0]["content"]
        self.assertEqual(content[0]["source"]["media_type"], "image/jpeg")
        self.assertNotIn("Dominant colors", content[1]["text"])
        doc = self.output()
        self.assertEqual(doc["documents"][0]["root"]["box"]["w"], 40)
        self.assertTrue(any(i["rule"] == "inference-hints" and "JPEG" in i["message"] for i in doc["lint"]))
        self.assertIn("preview", doc["capabilities"])

    def test_sketch_prompt(self):
        image = self.write("sketch.png", tiny_png())
        _, _, _, client = self.run_cli(["--image", image, "--sketch"], [{"json": reply_doc()}])
        text = client.calls[0]["messages"][0]["content"][1]["text"]
        self.assertIn("wireframe", text)
        self.assertNotIn("Dominant colors", text)  # sketch colors are meaningless

    def test_not_an_image(self):
        image = self.write("shot.png", b"GIF89a....")
        code, _, stderr, _ = self.run_cli(["--image", image], [{"json": reply_doc()}])
        self.assertEqual(code, infer.EXIT_USAGE)
        self.assertIn("not a PNG or JPEG", stderr)


class Tokens(Base):
    TOKENS = {"color": {"$type": "color",
                        "primary": {"$value": "#3366ff"},
                        "accent": {"$value": "{color.primary}"},
                        "white": {"$value": {"colorSpace": "srgb", "components": [1, 1, 1], "alpha": 1}}},
              "space": {"md": {"$value": "8px", "$type": "dimension"}}}

    def run_tokens(self, extra=(), doc=None):
        tokens = self.write("tokens.json", self.TOKENS)
        children = [
            node("near", "Near", fills=[{"type": "solid", "color": [0.21, 0.41, 0.98, 0.5]}], confidence=1, inferred=[]),
            node("far", "Far", fills=[{"type": "solid", "color": [0.9, 0.1, 0.1, 1]}], confidence=1, inferred=[]),
            node("label", "Label", "text", text={"content": "Hi", "color": [0.99, 0.99, 0.98, 1]}, confidence=1, inferred=[],
                 tokens={"gap": "space.md", "padding": "space.huge"}),
            node("ref", "Ref", fills=[{"type": "solid", "color": [0, 0, 0, 1], "colorToken": "color.nope"}], confidence=1, inferred=[]),
        ]
        code, stdout, _, client = self.run_cli(["--text", "x", "--tokens", tokens, *extra], [{"json": doc or reply_doc(children=children)}])
        self.assertEqual(code, 0, stdout)
        return self.output(), client

    def test_snapping(self):
        doc, client = self.run_tokens()
        self.assertIn("tokens", doc["capabilities"])
        self.assertEqual(doc["tokens"], self.TOKENS)
        near, far, label, ref = doc["documents"][0]["root"]["children"]
        self.assertEqual(near["fills"][0]["colorToken"], "color.primary")
        self.assertEqual(near["fills"][0]["color"], [0.2, 0.4, 1.0, 0.5])  # token rgb, original alpha
        self.assertIn("colorToken", near["inferred"])
        self.assertNotIn("colorToken", far["fills"][0])
        self.assertEqual(far["fills"][0]["color"], [0.9, 0.1, 0.1, 1])
        self.assertEqual(label["text"]["colorToken"], "color.white")
        self.assertEqual(label["tokens"], {"gap": "space.md"})  # undefined space.huge dropped
        self.assertNotIn("colorToken", ref["fills"][0])  # undefined color.nope dropped
        self.assertTrue(any(i["rule"] == "inference-token" and "color.nope" in i["message"] for i in doc["lint"]))
        system = client.calls[0]["system"][0]["text"]
        self.assertIn("color.primary", system)
        self.assertIn("colorToken", system)

    def test_no_snap_and_distance(self):
        doc, _ = self.run_tokens(["--no-snap"])
        self.assertNotIn("colorToken", doc["documents"][0]["root"]["children"][0]["fills"][0])
        doc, _ = self.run_tokens(["--snap-distance", "0.001"])
        self.assertNotIn("colorToken", doc["documents"][0]["root"]["children"][0]["fills"][0])

    def test_alias_and_distance_helpers(self):
        palette = infer.color_tokens(self.TOKENS)
        self.assertEqual(palette["color.accent"], palette["color.primary"])
        self.assertNotIn("space.md", palette)
        self.assertAlmostEqual(infer.color_distance([0, 0, 0], [1, 1, 1, 0]), 3 ** 0.5)


class Refusals(Base):
    def test_response_text_refusal(self):
        response = SimpleNamespace(stop_reason="refusal",
                                   stop_details=SimpleNamespace(type="refusal", category="cyber", explanation="Declined by policy"),
                                   content=[])
        with self.assertRaises(infer.Refusal) as ctx:
            infer.response_text(response)
        self.assertEqual(ctx.exception.category, "cyber")
        self.assertEqual(ctx.exception.code, infer.EXIT_REFUSAL)
        self.assertIn("Declined by policy", str(ctx.exception))

    def test_response_text_joins_text_blocks(self):
        response = SimpleNamespace(stop_reason="end_turn", stop_details=None, content=[
            SimpleNamespace(type="thinking", thinking=""), SimpleNamespace(type="text", text="{\"a\""),
            SimpleNamespace(type="fallback"), SimpleNamespace(type="text", text=": 1}")])
        self.assertEqual(infer.parse_reply(infer.response_text(response)), {"a": 1})

    def test_cli_refusal_exit(self):
        code, _, stderr, client = self.run_cli(["--text", "x"], [{"stop_reason": "refusal", "stop_details": {"category": None, "explanation": None}},
                                                                  {"json": reply_doc()}])
        self.assertEqual(code, infer.EXIT_REFUSAL)
        self.assertEqual(len(client.calls), 1)  # a refusal is not retried
        self.assertIn("declined", stderr)
        self.assertFalse(self.out.exists())


class Cache(Base):
    def test_cache_hit(self):
        argv = ["--text", "A settings screen"]
        code, _, _, first = self.run_cli(argv, [{"json": reply_doc()}])
        self.assertEqual(code, 0)
        self.assertEqual(len(first.calls), 1)
        self.assertEqual(len(list(self.cache.glob("*.json"))), 1)
        # Same inputs: the reply comes from the cache, the client is never called
        empty = infer.MockClient([])
        code, _, stderr, _ = self.run_cli(argv, [{"json": reply_doc()}], client=empty)
        self.assertEqual(code, 0, stderr)
        self.assertEqual(empty.calls, [])
        self.assertEqual(self.output()["source"]["x-inference"]["cache"], "hit")
        # Different effort: a different key
        other = infer.MockClient([{"json": reply_doc()}])
        self.run_cli(argv + ["--effort", "medium"], [{"json": reply_doc()}], client=other)
        self.assertEqual(len(other.calls), 1)
        # --no-cache bypasses it
        bypass = infer.MockClient([{"json": reply_doc()}])
        self.run_cli(argv + ["--no-cache"], [{"json": reply_doc()}], client=bypass)
        self.assertEqual(len(bypass.calls), 1)

    def test_default_location(self):
        args = infer.build_parser().parse_args(["--text", "x", "-o", "o.json"])
        with mock.patch.dict("os.environ", {"LOCALAPPDATA": r"C:\Users\u\AppData\Local"}, clear=False):
            with mock.patch.dict("os.environ", {infer.CACHE_ENV: ""}):
                self.assertEqual(infer.cache_dir(args), Path(r"C:\Users\u\AppData\Local") / "dwo-Forge" / "canvas-inference-cache")


class Sdk(Base):
    def test_missing_anthropic(self):
        with mock.patch.dict(sys.modules, {"anthropic": None}):  # import anthropic -> ImportError
            code, _, stderr, _ = self.run_cli(["--text", "x", "--no-cache"])
        self.assertEqual(code, infer.EXIT_USAGE)
        self.assertIn("pip install anthropic", stderr)
        self.assertFalse(self.out.exists())

    def test_sdk_errors_most_specific_first(self):
        class APIConnectionError(Exception):
            pass

        class APIStatusError(Exception):
            def __init__(self, status_code):
                super().__init__(f"status {status_code}")
                self.status_code = status_code
                self.message = f"status {status_code}"

        class RateLimitError(APIStatusError):
            pass

        sdk = SimpleNamespace(RateLimitError=RateLimitError, APIStatusError=APIStatusError, APIConnectionError=APIConnectionError)

        def client_raising(exc):
            def stream(**_):
                raise exc
            return SimpleNamespace(beta=SimpleNamespace(messages=SimpleNamespace(stream=stream)))

        for exc, expected in ((RateLimitError(429), "Rate limited"), (APIStatusError(500), "API error 500"),
                              (APIConnectionError("down"), "Could not reach")):
            with self.assertRaises(infer.InferenceError) as ctx:
                infer.stream_message(client_raising(exc), {}, sdk)
            self.assertIn(expected, str(ctx.exception))


class Sample(Base):
    def test_dwo_attribute_sample_reproduces(self):
        code, stdout, _, _ = self.run_cli(["--image", str(SAMPLES / "dwo_attribute.png"), "--name", "AttributeScreen",
                                           "--tokens", str(SAMPLES / "dwo_tokens.json"), "--no-cache",
                                           "--mock", str(SAMPLES / "dwo_attribute.mock.json")])
        self.assertEqual(code, 0, stdout)
        doc = self.output()
        self.assertFalse(has_errors(validate(doc)))
        committed = json.loads((SAMPLES / "dwo_attribute.draft.dwocanvas.json").read_text(encoding="utf-8"))
        for d in (doc, committed):
            d["source"].pop("exportedAt", None)
        self.assertEqual(doc, committed)


class Png(unittest.TestCase):
    def test_roundtrip(self):
        png = tiny_png(9, 5)
        image = pngtools.decode_png(png)
        self.assertEqual((image.width, image.height), (9, 5))
        self.assertEqual(pngtools.png_size(png), (9, 5))
        self.assertEqual(image.pixel(5, 2), (255, 170, 60, 255))
        colors = pngtools.dominant_colors(image)
        self.assertEqual(colors[0]["hex"], "#2850c8")

    def test_all_filters_rgb(self):
        """RGB (colour type 2) rows encoded with filters 0..4 decode to the original pixels."""
        import struct
        import zlib
        width, height, bpp = 5, 5, 3
        rows = [bytes((x * 40 + y * 7) % 256 for x in range(width * bpp)) for y in range(height)]

        def paeth(a, b, c):
            p = a + b - c
            pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

        raw = bytearray()
        prev = bytes(width * bpp)
        for y, row in enumerate(rows):
            f = y % 5
            out = bytearray()
            for i, v in enumerate(row):
                a = row[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                pred = [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]
                out.append((v - pred) & 0xFF)
            raw += bytes([f]) + out
            prev = row

        def chunk(kind, body):
            return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

        png = (pngtools.PNG_MAGIC + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
               + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b""))
        image = pngtools.decode_png(png)
        for y in range(height):
            for x in range(width):
                self.assertEqual(image.pixel(x, y), tuple(rows[y][x * 3:x * 3 + 3]) + (255,))

    def test_dwo_preview_decodes(self):
        image = pngtools.decode_png((SAMPLES / "dwo_attribute.png").read_bytes())
        self.assertEqual((image.width, image.height), (1600, 900))

    def test_sniff(self):
        self.assertEqual(pngtools.sniff(tiny_png()), "image/png")
        self.assertEqual(pngtools.sniff(b"\xff\xd8\xff\xe0"), "image/jpeg")
        self.assertIsNone(pngtools.sniff(b"GIF89a"))


if __name__ == "__main__":
    unittest.main()

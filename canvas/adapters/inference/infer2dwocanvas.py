#!/usr/bin/env python3
"""DwoCanvas inference adapter: screenshot / mockup, text description or sketch -> draft dwocanvas/2 document.

Design: canvas/docs/SchemaFirstArchitecture.md section 5.1. Usage and policy notes: README.md next to this file.

    python infer2dwocanvas.py --image shot.png -o Screen.dwocanvas.json [--name Screen] [--tokens tokens.json]
    python infer2dwocanvas.py --text "Settings screen with ..." -o Settings.dwocanvas.json
    python infer2dwocanvas.py --image shot.png -o out.json --mock responses.json     (no API call)

Pipeline: inputs -> image hints (PNG size, dominant colors) -> prompt (schema + rules + tokens) -> Claude API
(streaming, server-side refusal fallback) -> parse -> post-process -> dwocanvas validator; validation errors are
sent back to the model (validation loop, --max-retries) -> token snapping -> write + lint report.

Standard library only, except the ``anthropic`` SDK, imported lazily for real API calls.
"""

from __future__ import annotations

import argparse
import base64
import copy
import datetime as _dt
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from types import SimpleNamespace
from typing import Any, Callable

HERE = Path(__file__).resolve().parent
PYTHON_DIR = HERE.parents[1] / "python"  # canvas/python (dwocanvas validator)
for _p in (str(HERE), str(PYTHON_DIR)):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import pngtools  # noqa: E402
import prompt as prompts  # noqa: E402
from dwocanvas.schema import load_schema  # noqa: E402
from dwocanvas.validate import LEVELS, count_levels, has_errors, validate  # noqa: E402

ADAPTER = "dwocanvas-inference/0.1.0"
PROMPT_VERSION = prompts.PROMPT_VERSION
DEFAULT_MODEL = "claude-opus-5-5"
DEFAULT_EFFORT = "high"
MAX_TOKENS = 64000
FALLBACK_BETA = "server-side-fallback-2026-07-01"
DEFAULT_SIZE = (1280, 720)
#: sRGB Euclidean distance (channels 0..1, alpha ignored) below which a literal color snaps to a token color.
#: 0.06 ~ a difference of 9/255 on every channel, or 15/255 on one: invisible-to-subtle differences only.
DEFAULT_SNAP_DISTANCE = 0.06
LOW_CONFIDENCE = 0.5
PREVIEW_ASSET = "inference_preview"
CACHE_ENV = "DWOCANVAS_INFERENCE_CACHE"
MAX_FEEDBACK_ITEMS = 40

KNOWN_TOP = {"schema", "source", "capabilities", "tokens", "assets", "documents", "components", "lint", "usage"}

EXIT_OK = 0
EXIT_INVALID = 1     # the final document still has validator errors / no parsable reply after the retries
EXIT_USAGE = 2       # bad arguments, unreadable input, missing SDK, API / network error
EXIT_REFUSAL = 3     # the model (and the fallback chain) declined
EXIT_TRUNCATED = 4   # every attempt hit max_tokens


class InferenceError(Exception):
    def __init__(self, message: str, code: int = EXIT_USAGE):
        super().__init__(message)
        self.code = code


class Refusal(InferenceError):
    def __init__(self, category, explanation):
        text = "The model declined the request (stop_reason 'refusal')"
        text += f": category {category!r}" if category else ": no category given"
        if explanation:
            text += f", explanation: {explanation}"
        super().__init__(text, EXIT_REFUSAL)
        self.category = category
        self.explanation = explanation


class Truncated(InferenceError):
    def __init__(self):
        super().__init__(f"The reply hit max_tokens ({MAX_TOKENS}) and was truncated", EXIT_TRUNCATED)


# ----------------------------------------------------------------------------------------------------------------------
# OCR hook

#: Optional OCR engine: ``fn(image_bytes, media_type) -> [{"text": str, "box": [x, y, w, h]}]``. None = no OCR (the
#: model reads text from the image itself). Register a function with :func:`register_ocr` (e.g. from a wrapper script
#: that imports an in-house engine); results are given to the model as reliable hints.
OCR_ENGINE: Callable[[bytes, str], list[dict]] | None = None


def register_ocr(fn: Callable[[bytes, str], list[dict]] | None) -> None:
    global OCR_ENGINE
    OCR_ENGINE = fn


# ----------------------------------------------------------------------------------------------------------------------
# Inputs


class Inputs(SimpleNamespace):
    """mode ("image" | "sketch" | "text"), image (bytes | None), media_type, text, name, width, height, tokens, ..."""


def read_inputs(args) -> Inputs:
    image = media_type = None
    text = args.text
    source_file = None
    if args.image:
        try:
            image = Path(args.image).read_bytes()
        except OSError as e:
            raise InferenceError(f"cannot read image {args.image}: {e.strerror or e}") from e
        media_type = pngtools.sniff(image)
        if media_type is None:
            raise InferenceError(f"{args.image}: not a PNG or JPEG file (checked the magic bytes)")
        source_file = Path(args.image).name
    if args.text_file:
        try:
            text = Path(args.text_file).read_text(encoding="utf-8")
        except OSError as e:
            raise InferenceError(f"cannot read {args.text_file}: {e.strerror or e}") from e
        source_file = source_file or Path(args.text_file).name
    if image is None and not (text and text.strip()):
        raise InferenceError("give --image, --text or --text-file")
    if args.sketch and image is None:
        raise InferenceError("--sketch needs --image (the sketch)")

    tokens = None
    if args.tokens:
        try:
            tokens = json.loads(Path(args.tokens).read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise InferenceError(f"cannot read tokens {args.tokens}: {e}") from e
        if not isinstance(tokens, dict):
            raise InferenceError(f"{args.tokens}: design tokens must be a JSON object (W3C format)")

    mode = "text" if image is None else ("sketch" if args.sketch else "image")
    return Inputs(mode=mode, image=image, media_type=media_type, text=text, name=args.name, tokens=tokens,
                  width=args.width, height=args.height, source_file=source_file)


def analyse_image(inputs: Inputs, notes: list[dict]) -> None:
    """Fills inputs.width/height (if not given), inputs.decoded (PNG pixels or None) and inputs.hints."""
    inputs.decoded = None
    inputs.hints = []
    size = None
    if inputs.image is not None:
        if inputs.media_type == "image/png":
            size = pngtools.png_size(inputs.image)
            try:
                inputs.decoded = pngtools.decode_png(inputs.image)
            except Exception as e:  # noqa: BLE001  (PngError, zlib.error, truncated files)
                notes.append(note("info", "inference-hints", f"PNG not decoded ({e}): no color hints, no icon crops"))
        else:
            size = pngtools.jpeg_size(inputs.image)
            notes.append(note("info", "inference-hints",
                              "JPEG input: pixels are not decoded (standard library only), so no color hints and no "
                              "icon crops. Convert to PNG to get them."))
        if size:
            inputs.hints.append(f"Image size: {size[0]} x {size[1]} px.")
    if inputs.width is None or inputs.height is None:
        w, h = size if size else DEFAULT_SIZE
        inputs.width = inputs.width if inputs.width is not None else w
        inputs.height = inputs.height if inputs.height is not None else h
    if size and (size[0], size[1]) != (inputs.width, inputs.height):
        inputs.hints.append(f"The canvas is {inputs.width:g} x {inputs.height:g}: scale image measurements by "
                            f"{inputs.width / size[0]:.4g} horizontally and {inputs.height / size[1]:.4g} vertically.")
    if inputs.decoded is not None and inputs.mode == "image":
        colors = pngtools.dominant_colors(inputs.decoded)
        if colors:
            inputs.hints.append("Dominant colors (share of the image): " + ", ".join(
                f"{c['hex']} = {c['rgb']} ({c['share'] * 100:.1f}%)" for c in colors))
    if inputs.image is not None and OCR_ENGINE is not None:
        try:
            words = OCR_ENGINE(inputs.image, inputs.media_type) or []
        except Exception as e:  # noqa: BLE001  (third-party engine)
            words = []
            notes.append(note("warning", "inference-ocr", f"OCR engine failed: {e}"))
        if words:
            inputs.hints.append("OCR text (box = x, y, w, h in image px): " + "; ".join(
                f"{w.get('text', '')!r} at {w.get('box')}" for w in words[:200]))


# ----------------------------------------------------------------------------------------------------------------------
# Request


def build_request(inputs: Inputs, args) -> dict:
    """Keyword arguments for ``client.beta.messages.stream`` (without the retry turns)."""
    system = prompts.system_prompt(load_schema(), inputs.tokens)
    content: list[dict] = []
    if inputs.image is not None:
        content.append({"type": "image", "source": {"type": "base64", "media_type": inputs.media_type,
                                                    "data": base64.b64encode(inputs.image).decode("ascii")}})
    content.append({"type": "text", "text": prompts.user_text(
        mode=inputs.mode, name=inputs.name, width=inputs.width, height=inputs.height,
        description=inputs.text, hints=inputs.hints)})
    request = {
        "model": args.model,
        "max_tokens": MAX_TOKENS,
        "output_config": {"effort": args.effort},
        # Stable prefix (schema + rules + tokens): cached across the validation-loop retries
        "system": [{"type": "text", "text": system, "cache_control": {"type": "ephemeral"}}],
        "messages": [{"role": "user", "content": content}],
    }
    if not args.no_fallback:
        request["betas"] = [FALLBACK_BETA]
        request["fallbacks"] = "default"
    return request


def response_text(response) -> str:
    """Text of a Messages API response; raises :class:`Refusal` / :class:`Truncated` by ``stop_reason``."""
    stop = getattr(response, "stop_reason", None)
    if stop == "refusal":
        details = getattr(response, "stop_details", None)
        raise Refusal(getattr(details, "category", None), getattr(details, "explanation", None))
    if stop == "max_tokens":
        raise Truncated()
    return "".join(getattr(b, "text", "") for b in (getattr(response, "content", None) or [])
                   if getattr(b, "type", None) == "text")


FENCE = re.compile(r"```(?:json)?\s*\n(.*?)\n?```", re.S)


def parse_reply(text: str) -> Any:
    """json.loads of the reply, after stripping a ```json fence (or prose around the outermost object)."""
    body = text.strip()
    match = FENCE.search(body)
    if match:
        body = match.group(1).strip()
    elif not body.startswith("{"):
        start, end = body.find("{"), body.rfind("}")
        if start != -1 and end > start:
            body = body[start:end + 1]
    return json.loads(body)


def import_anthropic():
    try:
        import anthropic  # noqa: PLC0415  (lazy: only real API calls need it)
    except ImportError as e:
        raise InferenceError("The 'anthropic' package is not installed. Install it with: pip install anthropic "
                             "(or run with --mock <responses.json> to test without the API)") from e
    return anthropic


def make_client():
    anthropic = import_anthropic()
    # Credentials: ANTHROPIC_API_KEY, or an `ant auth login` profile (resolved by the SDK)
    return anthropic.Anthropic(), anthropic


def stream_message(client, request: dict, sdk=None):
    """One streamed request; SDK errors become :class:`InferenceError` (most specific first)."""
    def call():
        with client.beta.messages.stream(**request) as stream:
            return stream.get_final_message()

    if sdk is None:  # mock client
        return call()
    try:
        return call()
    except sdk.RateLimitError as e:
        raise InferenceError(f"Rate limited by the API (429), retries exhausted: {e}. Try again later.") from e
    except sdk.APIStatusError as e:
        raise InferenceError(f"API error {e.status_code}: {e.message if hasattr(e, 'message') else e}") from e
    except sdk.APIConnectionError as e:
        raise InferenceError(f"Could not reach the API: {e}") from e


# ----------------------------------------------------------------------------------------------------------------------
# Mock client (same pipeline, canned replies)


class MockClient:
    """Stands in for ``anthropic.Anthropic``: ``beta.messages.stream(**kw)`` returns the next canned reply.

    Mock file: a JSON list; one item per API call, in order. An item is a string (the assistant text), or an object
    ``{"text": "..."}`` / ``{"json": <document>}`` (serialized for you), with optional ``"stop_reason"`` (default
    "end_turn") and ``"stop_details": {"category", "explanation"}``. ``calls`` records every request.
    """

    def __init__(self, replies: list, model: str = DEFAULT_MODEL):
        self.replies = list(replies)
        self.calls: list[dict] = []
        self.model = model
        self.beta = SimpleNamespace(messages=SimpleNamespace(stream=self._stream))

    @classmethod
    def from_file(cls, path: str, model: str = DEFAULT_MODEL) -> "MockClient":
        try:
            replies = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise InferenceError(f"cannot read mock responses {path}: {e}") from e
        if not isinstance(replies, list):
            raise InferenceError(f"{path}: mock responses must be a JSON list")
        return cls(replies, model)

    def _stream(self, **request):
        self.calls.append(copy.deepcopy(request))
        if len(self.calls) > len(self.replies):
            raise InferenceError(f"mock responses exhausted: call {len(self.calls)} but only {len(self.replies)} canned replies")
        item = self.replies[len(self.calls) - 1]
        response = self.make_response(item, request.get("model", self.model))

        class _Stream:
            def __enter__(self_inner):
                return self_inner

            def __exit__(self_inner, *exc):
                return False

            def get_final_message(self_inner):
                return response

        return _Stream()

    @staticmethod
    def make_response(item, model: str):
        if isinstance(item, str):
            item = {"text": item}
        if not isinstance(item, dict):
            raise InferenceError("mock reply must be a string or an object")
        text = item.get("text")
        if text is None and "json" in item:
            text = "```json\n" + json.dumps(item["json"], ensure_ascii=False, indent=1) + "\n```"
        details = item.get("stop_details")
        return SimpleNamespace(
            model=model,
            stop_reason=item.get("stop_reason", "end_turn"),
            stop_details=SimpleNamespace(**details) if isinstance(details, dict) else None,
            content=[SimpleNamespace(type="text", text=text or "")],
        )


# ----------------------------------------------------------------------------------------------------------------------
# Post-processing


def note(level: str, rule: str, message: str, node: dict | None = None) -> dict:
    return {"level": level, "node": str(node.get("id", "")) if node else "", "name": str(node.get("name", "")) if node else "",
            "message": message, "rule": rule}


def walk(node: Any, parent: dict | None = None, origin=(0.0, 0.0)):
    """(node, parent, absolute x, absolute y) depth-first."""
    if not isinstance(node, dict):
        return
    box = node.get("box") if isinstance(node.get("box"), dict) else {}
    x = origin[0] + _num(box.get("x"))
    y = origin[1] + _num(box.get("y"))
    yield node, parent, x, y
    for child in node.get("children") or []:
        yield from walk(child, node, (x, y))


def _num(v) -> float:
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else 0.0


def all_documents(doc: dict):
    for key in ("documents", "components"):
        for d in doc.get(key) or []:
            if isinstance(d, dict):
                yield key, d


def normalize_shape(doc: Any, inputs: Inputs) -> dict:
    """Accept a bare document / root node and wrap it; anything else stays for the validator to reject."""
    if isinstance(doc, dict) and "documents" not in doc:
        if "root" in doc:
            doc = {"documents": [doc]}
        elif "type" in doc and ("children" in doc or "box" in doc):
            doc = {"documents": [{"id": "screen", "name": inputs.name, "root": doc}]}
    return doc if isinstance(doc, dict) else {"documents": doc}


def unique_ids(doc: dict) -> int:
    """Gives every node a unique id within its document and every document a unique id. Returns changes made."""
    changed = 0
    doc_ids: set[str] = set()
    for key, d in all_documents(doc):
        did = d.get("id") if isinstance(d.get("id"), str) and d.get("id") else None
        base = did or ("screen" if key == "documents" else "component")
        candidate, n = base, 2
        while candidate in doc_ids:
            candidate, n = f"{base}~{n}", n + 1
        if candidate != did:
            d["id"] = candidate
            changed += 1
        doc_ids.add(candidate)
        seen: set[str] = set()
        counter = 0
        for node, _, _, _ in walk(d.get("root")):
            nid = node.get("id")
            if isinstance(nid, (int, float)) and not isinstance(nid, bool):
                nid = str(nid)
            if not isinstance(nid, str) or not nid:
                counter += 1
                nid = f"n{counter}"
            candidate, n = nid, 2
            while candidate in seen:
                candidate, n = f"{nid}~{n}", n + 1
            if candidate != node.get("id"):
                node["id"] = candidate
                changed += 1
            seen.add(candidate)
    return changed


def default_confidence(doc: dict) -> int:
    """confidence missing / invalid -> 0.5 and "confidence" added to inferred. Returns the number of nodes fixed."""
    fixed = 0
    for _, d in all_documents(doc):
        for node, _, _, _ in walk(d.get("root")):
            inferred = node.get("inferred")
            inferred = [str(f) for f in inferred if isinstance(f, (str, int, float))] if isinstance(inferred, list) else []
            c = node.get("confidence")
            if not isinstance(c, (int, float)) or isinstance(c, bool):
                node["confidence"] = 0.5
                if "confidence" not in inferred:
                    inferred.append("confidence")
                fixed += 1
            else:
                node["confidence"] = min(1.0, max(0.0, float(c)))
            node["inferred"] = list(dict.fromkeys(inferred))
    return fixed


def color_tokens(tokens: dict | None) -> dict[str, list[float]]:
    """``{path: [r, g, b, a]}`` for color tokens (hex strings, {components} objects, arrays; aliases followed)."""
    if not tokens:
        return {}
    flat = {path: (value, ttype) for path, value, ttype in prompts.flatten_tokens(tokens)}

    def resolve(value, depth=0):
        if isinstance(value, str) and value.startswith("{") and value.endswith("}") and depth < 10:
            target = flat.get(value[1:-1])
            return resolve(target[0], depth + 1) if target else None
        return parse_color(value)

    out = {}
    for path, (value, ttype) in flat.items():
        if ttype and ttype != "color":
            continue
        rgba = resolve(value)
        if rgba is not None:
            out[path] = rgba
    return out


def parse_color(value) -> list[float] | None:
    if isinstance(value, str):
        h = value.strip().lstrip("#")
        if re.fullmatch(r"[0-9a-fA-F]{3,4}", h):
            h = "".join(c * 2 for c in h)
        if re.fullmatch(r"[0-9a-fA-F]{6}([0-9a-fA-F]{2})?", h):
            parts = [int(h[i:i + 2], 16) / 255 for i in range(0, len(h), 2)]
            return (parts + [1.0])[:4]
        return None
    if isinstance(value, dict):  # DTCG 2025: {"colorSpace": "srgb", "components": [r, g, b], "alpha": a}
        comps = value.get("components")
        if value.get("colorSpace", "srgb") == "srgb" and isinstance(comps, list) and len(comps) == 3:
            return [float(c) for c in comps] + [float(value.get("alpha", 1.0))]
        if isinstance(value.get("hex"), str):
            return parse_color(value["hex"])
        return None
    if isinstance(value, list) and len(value) in (3, 4) and all(isinstance(c, (int, float)) for c in value):
        return [float(c) for c in value] + ([1.0] if len(value) == 3 else [])
    return None


def color_distance(a, b) -> float:
    """sRGB Euclidean distance over r, g, b in 0..1 (alpha ignored)."""
    return sum((float(a[i]) - float(b[i])) ** 2 for i in range(3)) ** 0.5


def _color_slots(node: dict):
    """(holder dict) for every literal color with an optional colorToken: solid fills, stroke, text (and states)."""
    visuals = [node]
    if isinstance(node.get("states"), dict):
        visuals += [v for v in node["states"].values() if isinstance(v, dict)]
    for v in visuals:
        for paint in v.get("fills") or []:
            if isinstance(paint, dict) and paint.get("type") == "solid":
                yield paint
        if isinstance(v.get("stroke"), dict):
            yield v["stroke"]
    if isinstance(node.get("text"), dict):
        yield node["text"]


def apply_tokens(doc: dict, tokens: dict | None, snap: bool, distance: float, notes: list[dict]) -> int:
    """Drops references to undefined tokens; snaps literal colors to the nearest color token. Returns snaps made."""
    defined = {path for path, _, _ in prompts.flatten_tokens(tokens)} if tokens else set()
    palette = color_tokens(tokens)
    snapped = 0
    for _, d in all_documents(doc):
        for node, _, _, _ in walk(d.get("root")):
            refs = node.get("tokens")
            if isinstance(refs, dict):
                for prop, path in list(refs.items()):
                    if path not in defined:
                        del refs[prop]
                        notes.append(note("warning", "inference-token", f"Dropped reference to undefined token '{path}' ({prop})", node))
                if not refs:
                    del node["tokens"]
            for holder in _color_slots(node):
                token = holder.get("colorToken")
                if token is not None and token not in defined:
                    del holder["colorToken"]
                    notes.append(note("warning", "inference-token", f"Dropped reference to undefined color token '{token}'", node))
                    token = None
                color = holder.get("color")
                if token is None and snap and palette and isinstance(color, list) and len(color) >= 3:
                    path, best = min(((p, color_distance(color, c)) for p, c in palette.items()), key=lambda t: t[1])
                    if best <= distance:
                        rgba = palette[path]
                        holder["color"] = [round(c, 4) for c in rgba[:3]] + [color[3] if len(color) > 3 else rgba[3]]
                        holder["colorToken"] = path
                        snapped += 1
                        if isinstance(node.get("inferred"), list) and "colorToken" not in node["inferred"]:
                            node["inferred"].append("colorToken")
    return snapped


def capabilities_for(doc: dict, inputs: Inputs) -> list[str]:
    caps = ["inferred", "layout.flow"]
    gradients = images = effects = constraints = False
    for _, d in all_documents(doc):
        for node, _, _, _ in walk(d.get("root")):
            for paint in node.get("fills") or []:
                if isinstance(paint, dict):
                    gradients |= paint.get("type") in ("linear", "radial", "angular", "diamond")
                    images |= paint.get("type") == "image"
            effects |= bool(node.get("effects"))
            constraints |= isinstance(node.get("constraints"), dict)
    if constraints:
        caps.append("layout.constraints")
    if gradients:
        caps.append("paint.gradient")
    if images:
        caps.append("paint.image")
    if effects:
        caps.append("effects")
    if doc.get("components"):
        caps.append("components")
    if inputs.tokens:
        caps.append("tokens")
    if inputs.image is not None:
        caps.append("preview")
    return caps


def bake_crops(doc: dict, inputs: Inputs, enabled: bool, notes: list[dict]) -> int:
    """``bake.image == "crop"`` -> crop that box out of the source PNG as a new asset. Returns crops made."""
    made = 0
    assets = doc.setdefault("assets", {})
    sx = (inputs.decoded.width / inputs.width) if inputs.decoded is not None and inputs.width else 1.0
    sy = (inputs.decoded.height / inputs.height) if inputs.decoded is not None and inputs.height else 1.0
    for key, d in all_documents(doc):
        for node, _, ax, ay in walk(d.get("root")):
            bake = node.get("bake")
            if not isinstance(bake, dict) or bake.get("image") in assets:
                continue
            box = node.get("box") if isinstance(node.get("box"), dict) else None
            region = None
            if enabled and key == "documents" and inputs.decoded is not None and box:
                region = pngtools.crop(inputs.decoded, ax * sx, ay * sy, _num(box.get("w")) * sx, _num(box.get("h")) * sy)
            if region is None:
                del node["bake"]
                why = "no decodable PNG source" if inputs.decoded is None else ("--no-crop" if not enabled else "empty or out-of-image box")
                notes.append(note("warning", "inference-crop", f"Bake request dropped ({why}): drawn as a plain shape", node))
                continue
            data = pngtools.encode_png(region)
            asset_id = f"crop_{hashlib.sha256(data).hexdigest()[:12]}"
            assets[asset_id] = {"data": base64.b64encode(data).decode("ascii"), "sha256": hashlib.sha256(data).hexdigest(), "scale": 1}
            bake["image"] = asset_id
            if isinstance(node.get("inferred"), list) and "bake" not in node["inferred"]:
                node["inferred"].append("bake")
            made += 1
    return made


def postprocess(raw: Any, inputs: Inputs, args, served_model: str | None = None) -> tuple[dict, list[dict]]:
    """Model reply -> draft document + adapter notes (lint items). Robust against malformed input."""
    notes: list[dict] = []
    doc = copy.deepcopy(normalize_shape(raw, inputs))
    for key in [k for k in doc if k not in KNOWN_TOP and not k.startswith("x-")]:
        del doc[key]
        notes.append(note("info", "inference-cleanup", f"Removed unknown top-level key '{key}' from the model reply"))
    for key in ("lint", "usage", "capabilities", "source"):
        doc.pop(key, None)
    # The model cannot produce image bytes: keep no asset it wrote
    doc["assets"] = {}
    doc["schema"] = "dwocanvas/2"
    model_label = f"{args.model}{' (mock)' if args.mock else ''}; prompt v{PROMPT_VERSION}"
    source = {"tool": "inference", "adapter": ADAPTER, "mode": "draft", "model": model_label,
              "exportedAt": _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
              "x-inference": {"input": inputs.mode, "effort": args.effort, "fallback": not args.no_fallback,
                              "promptVersion": PROMPT_VERSION}}
    if served_model and served_model != args.model:
        source["x-inference"]["servedModel"] = served_model
    if inputs.source_file:
        source["file"] = inputs.source_file
    doc = {"schema": doc.pop("schema"), "source": source, **doc}

    # Only the adapter knows the preview: drop any the model wrote, then set the source image on the screen
    for _, d in all_documents(doc):
        d.pop("preview", None)
    documents = doc.get("documents")
    if isinstance(documents, list) and documents and isinstance(documents[0], dict):
        first = documents[0]
        first["name"] = inputs.name
        first.setdefault("kind", "screen")
        root = first.get("root")
        if isinstance(root, dict):
            box = root.get("box") if isinstance(root.get("box"), dict) else None
            if inputs.image is not None or box is None:
                root["box"] = {"x": 0, "y": 0, "w": inputs.width, "h": inputs.height}
        if inputs.image is not None:
            doc["assets"][PREVIEW_ASSET] = {"data": base64.b64encode(inputs.image).decode("ascii"),
                                            "sha256": hashlib.sha256(inputs.image).hexdigest(), "scale": 1}
            first["preview"] = PREVIEW_ASSET

    changed = unique_ids(doc)
    if changed:
        notes.append(note("info", "inference-ids", f"Assigned {changed} missing or duplicate id(s)"))
    defaulted = default_confidence(doc)
    if defaulted:
        notes.append(note("info", "inference-confidence", f"{defaulted} node(s) had no confidence: set to 0.5 (marked inferred)"))
    crops = bake_crops(doc, inputs, not args.no_crop, notes)
    if crops:
        notes.append(note("info", "inference-crop", f"Cropped {crops} icon region(s) from the source image as baked assets"))
    snapped = apply_tokens(doc, inputs.tokens, not args.no_snap, args.snap_distance, notes)
    if inputs.tokens:
        doc["tokens"] = inputs.tokens
        notes.append(note("info", "inference-token", f"Snapped {snapped} literal color(s) to design tokens "
                                                      f"(sRGB distance <= {args.snap_distance:g})"))
    else:
        doc.pop("tokens", None)
    if not doc["assets"]:
        del doc["assets"]
    doc["capabilities"] = capabilities_for(doc, inputs)
    # Low-confidence nodes first in the report: they are what a reviewer should look at
    low = []
    for _, d in all_documents(doc):
        for node, _, _, _ in walk(d.get("root")):
            if isinstance(node.get("confidence"), float) and node["confidence"] < LOW_CONFIDENCE:
                low.append(node)
    for node in sorted(low, key=lambda n: n["confidence"]):
        guessed = ", ".join(node.get("inferred") or []) or "-"
        notes.append(note("info", "inference-review", f"Low confidence {node['confidence']:.2f}; guessed: {guessed}", node))
    return doc, notes


# ----------------------------------------------------------------------------------------------------------------------
# Cache


def cache_dir(args) -> Path:
    if args.cache_dir:
        return Path(args.cache_dir)
    if os.environ.get(CACHE_ENV):
        return Path(os.environ[CACHE_ENV])
    base = os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), ".cache")
    return Path(base) / "dwo-Forge" / "canvas-inference-cache"


def cache_key(request: dict, inputs: Inputs, args) -> str:
    """sha256 over the inputs (image bytes, text, mode, size, tokens, name), model, prompt version and effort."""
    h = hashlib.sha256()
    parts = {
        "image": hashlib.sha256(inputs.image).hexdigest() if inputs.image is not None else None,
        "text": inputs.text, "mode": inputs.mode, "name": inputs.name,
        "size": [inputs.width, inputs.height], "tokens": inputs.tokens,
        "model": args.model, "prompt": PROMPT_VERSION, "effort": args.effort,
        "fallback": not args.no_fallback, "ocr": bool(OCR_ENGINE),
        "mock": hashlib.sha256(Path(args.mock).read_bytes()).hexdigest() if args.mock else None,
    }
    h.update(json.dumps(parts, sort_keys=True, ensure_ascii=False).encode("utf-8"))
    return h.hexdigest()


def cache_read(path: Path) -> dict | None:
    try:
        entry = json.loads(path.read_text(encoding="utf-8"))
        return entry if isinstance(entry, dict) and isinstance(entry.get("reply"), str) else None
    except (OSError, ValueError):
        return None


def cache_write(path: Path, entry: dict) -> None:
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(".tmp")
        tmp.write_text(json.dumps(entry, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, path)
    except OSError as e:
        print(f"warning: cache not written ({e})", file=sys.stderr)


# ----------------------------------------------------------------------------------------------------------------------
# Generation (validation loop)


def format_issue(issue: dict) -> str:
    where = f"{issue.get('node', '')} '{issue.get('name', '')}': " if issue.get("node") or issue.get("name") else ""
    return f"[{issue.get('rule', '-')}] {where}{issue.get('message', '')}"


def generate(client, sdk, request: dict, inputs: Inputs, args, log=print) -> tuple[dict, list[dict], dict]:
    """Ask, parse, post-process, validate; feed errors back up to ``--max-retries`` times.

    Returns (document, notes, info) for the last parsable reply (info: attempts, reply text, served model).
    Raises InferenceError when no attempt gave a parsable document.
    """
    messages = request["messages"]
    attempts = args.max_retries + 1
    best = None
    last_problem = "no reply"
    for attempt in range(1, attempts + 1):
        log(f"[{attempt}/{attempts}] requesting {args.model}{' (mock)' if args.mock else ''} (effort {args.effort})...")
        response = stream_message(client, {**request, "messages": messages}, sdk)
        try:
            text = response_text(response)
        except Truncated as e:
            last_problem = str(e)
            if attempt == attempts:
                raise
            log(f"  truncated at max_tokens: asking for a more compact document")
            messages[-1]["content"].append({"type": "text", "text": prompts.feedback_text([], "truncated")})
            continue
        served = getattr(response, "model", None)
        try:
            raw = parse_reply(text)
        except ValueError as e:
            last_problem = f"reply is not valid JSON: {e}"
            log(f"  {last_problem}")
            messages.append({"role": "assistant", "content": response.content})
            messages.append({"role": "user", "content": [{"type": "text", "text": prompts.feedback_text([str(e)], "json")}]})
            continue
        doc, notes = postprocess(raw, inputs, args, served)
        issues = validate(doc)
        errors = [i for i in issues if i.get("level") == "error"]
        best = (doc, notes, {"attempts": attempt, "reply": text, "servedModel": served, "issues": issues})
        if not errors:
            return best
        last_problem = f"{len(errors)} validator error(s)"
        log(f"  {last_problem}")
        problems = [format_issue(i) for i in errors[:MAX_FEEDBACK_ITEMS]]
        if len(errors) > MAX_FEEDBACK_ITEMS:
            problems.append(f"... and {len(errors) - MAX_FEEDBACK_ITEMS} more")
        messages.append({"role": "assistant", "content": response.content})
        messages.append({"role": "user", "content": [{"type": "text", "text": prompts.feedback_text(problems, "schema")}]})
    if best is not None:
        return best
    raise InferenceError(f"No usable document after {attempts} attempt(s): {last_problem}", EXIT_INVALID)


# ----------------------------------------------------------------------------------------------------------------------
# CLI


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="infer2dwocanvas", description="Screenshot / text / sketch -> draft dwocanvas/2 document (Claude API).")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--image", help="PNG or JPEG screenshot / mockup (or sketch with --sketch)")
    src.add_argument("--text", help="text description of the screen")
    src.add_argument("--text-file", help="Markdown / text file with the description")
    p.add_argument("--sketch", action="store_true", help="the image is a wireframe: layout and roles only, default styling")
    p.add_argument("-o", "--output", required=True, help="output .dwocanvas.json")
    p.add_argument("--name", default="Screen", help="document name (WBP name) [Screen]")
    p.add_argument("--width", type=float, help="canvas width in px [image width, else 1280]")
    p.add_argument("--height", type=float, help="canvas height in px [image height, else 720]")
    p.add_argument("--tokens", help="W3C design tokens JSON (preferred colors / spacing; enables token snapping)")
    p.add_argument("--snap-distance", type=float, default=DEFAULT_SNAP_DISTANCE,
                   help=f"max sRGB distance (0..1 channels) for snapping a color to a token [{DEFAULT_SNAP_DISTANCE}]")
    p.add_argument("--no-snap", action="store_true", help="do not snap literal colors to token colors")
    p.add_argument("--no-crop", action="store_true", help="do not crop icon regions from the image as baked assets")
    p.add_argument("--model", default=DEFAULT_MODEL, help=f"Claude model id [{DEFAULT_MODEL}]")
    p.add_argument("--effort", default=DEFAULT_EFFORT, choices=["low", "medium", "high", "xhigh", "max"], help=f"[{DEFAULT_EFFORT}]")
    p.add_argument("--max-retries", type=int, default=3, help="validation-loop retries after the first attempt [3]")
    p.add_argument("--no-fallback", action="store_true", help="disable the server-side refusal fallback (drops betas / fallbacks)")
    p.add_argument("--mock", help="JSON list of canned assistant replies, used in order instead of the API")
    p.add_argument("--no-cache", action="store_true", help="neither read nor write the reply cache")
    p.add_argument("--cache-dir", help=r"cache directory [%%LOCALAPPDATA%%\dwo-Forge\canvas-inference-cache]")
    p.add_argument("--quiet", action="store_true", help="only the final report")
    return p


def report(path: str, doc: dict, issues: list[dict]) -> str:
    lines = [f"{path} ({doc.get('schema')}, draft, {doc.get('source', {}).get('model', '')})"]
    review = [i for i in issues if i.get("rule") == "inference-review"]
    if review:
        lines.append(f"REVIEW FIRST: low-confidence nodes ({len(review)})")
        lines.extend("  " + format_issue(i) for i in review[:20])
    rest = [i for i in issues if i.get("rule") != "inference-review"]
    for level in LEVELS:
        group = [i for i in rest if i.get("level") == level]
        if group:
            lines.append(f"{level.upper()} ({len(group)})")
            lines.extend("  " + format_issue(i) for i in group)
    counts = count_levels(issues)
    lines.append(f"Summary: {counts['error']} error(s), {counts['warning']} warning(s), {counts['info']} info")
    return "\n".join(lines)


def run(args, client=None, log=None) -> int:
    log = log or ((lambda *a, **k: None) if args.quiet else (lambda *a, **k: print(*a, file=sys.stderr, **k)))
    if args.max_retries < 0:
        raise InferenceError("--max-retries must be >= 0")
    inputs = read_inputs(args)
    pre_notes: list[dict] = []
    analyse_image(inputs, pre_notes)
    request = build_request(inputs, args)

    key = cache_key(request, inputs, args)
    cpath = cache_dir(args) / f"{key}.json"
    cached = None if args.no_cache else cache_read(cpath)
    if cached is not None:
        log(f"cache hit: {cpath}")
        doc, notes = postprocess(parse_reply(cached["reply"]), inputs, args, cached.get("servedModel"))
        info = {"attempts": 0, "reply": cached["reply"], "servedModel": cached.get("servedModel")}
        doc["source"]["x-inference"]["cache"] = "hit"
    else:
        sdk = None
        if client is None:
            if args.mock:
                client = MockClient.from_file(args.mock, args.model)
            else:
                client, sdk = make_client()
        doc, notes, info = generate(client, sdk, request, inputs, args, log)
        doc["source"]["x-inference"]["attempts"] = info["attempts"]
        if not args.no_cache and not has_errors(info.get("issues", [])):
            cache_write(cpath, {"reply": info["reply"], "servedModel": info.get("servedModel"), "model": args.model,
                                "promptVersion": PROMPT_VERSION, "effort": args.effort,
                                "createdAt": _dt.datetime.now(_dt.timezone.utc).isoformat()})

    issues = validate(doc)
    doc["lint"] = pre_notes + notes + [{k: v for k, v in i.items() if k != "path"} for i in issues]
    # Writing lint into the document must not change its validity
    final = validate(doc)
    try:
        Path(args.output).parent.mkdir(parents=True, exist_ok=True)
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            json.dump(doc, f, ensure_ascii=False, indent=1)
            f.write("\n")
    except OSError as e:
        raise InferenceError(f"cannot write {args.output}: {e.strerror or e}") from e
    print(report(args.output, doc, doc["lint"]))
    if has_errors(final):
        print(f"The draft still has validator errors after {args.max_retries} retries: fix them by hand or re-run.", file=sys.stderr)
        return EXIT_INVALID
    return EXIT_OK


def main(argv=None, client=None) -> int:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8")
        except (AttributeError, ValueError):
            pass
    args = build_parser().parse_args(argv)
    try:
        return run(args, client)
    except InferenceError as e:
        print(f"error: {e}", file=sys.stderr)
        return e.code


if __name__ == "__main__":
    sys.exit(main())

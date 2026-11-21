"""designbridge/1 -> dwocanvas/2 migration.

Mirrors the importer's v1 -> v2 step (SchemaFirstArchitecture.md section 6). Pure function: the input is not modified.
A dwocanvas/2 input is returned as an unchanged deep copy, so migration is idempotent.
"""

from __future__ import annotations

import copy
import re
from typing import Any

from .schema import SCHEMA_V1, SCHEMA_V2

#: Capabilities an extracting adapter (Figma-like) provides. Migrated v1 files get these.
EXTRACT_CAPABILITIES = [
    "layout.flow",
    "layout.constraints",
    "paint.gradient",
    "paint.image",
    "effects",
    "components",
    "variants",
    "preview",
    "stable-ids",
]

#: v1 meta.role values (UE EDwoCanvasRole enum names) -> v2 role.
UE_ROLE_MAP = {
    "Button": "button",
    "ListView": "list",
    "TileView": "tile",
    "ScrollBox": "scroll",
    "TextInput": "textInput",
    "CheckBox": "checkBox",
    "Slider": "slider",
    "ProgressBar": "progressBar",
    "Switcher": "switcher",
    "NamedSlot": "namedSlot",
    "Spacer": "spacer",
    "Ignore": "ignore",
    "Auto": "auto",
}
V2_ROLES = frozenset(UE_ROLE_MAP.values())

V2_TOP_LEVEL = {"schema", "source", "capabilities", "tokens", "assets", "documents", "components", "lint", "usage"}

# Checked in order: compound names before the words they contain (semibold before bold, extralight before light).
_WEIGHTS = [
    (("extralight", "ultralight"), 200),
    (("semibold", "demibold"), 600),
    (("extrabold", "ultrabold", "heavy"), 800),
    (("thin", "hairline"), 100),
    (("light",), 300),
    (("medium",), 500),
    (("bold",), 700),
    (("black",), 900),
    (("regular", "book", "normal"), 400),
]


def font_weight(style: Any) -> tuple[int, bool]:
    """CSS weight and italic flag from a font style name ("SemiBold Italic" -> (600, True)). Unknown -> 400."""
    text = re.sub(r"[\s_\-]+", "", str(style or "")).lower()
    italic = "italic" in text or "oblique" in text
    for words, weight in _WEIGHTS:
        if any(w in text for w in words):
            return weight, italic
    return 400, italic


def map_role(value: Any) -> str | None:
    """v1 meta.role (UE enum name), or an already-v2 role / x- role -> v2 role. None if unknown."""
    if not isinstance(value, str) or not value:
        return None
    if value in UE_ROLE_MAP:
        return UE_ROLE_MAP[value]
    if value in V2_ROLES or re.fullmatch(r"x-[A-Za-z0-9_.-]+", value):
        return value
    return None


def truthy(value: Any) -> bool:
    """Like UE's FJsonValue::TryGetBool on v1 meta.variable (bool, or "true" / "yes" / "1" / "on")."""
    if isinstance(value, bool):
        return value
    if isinstance(value, str):
        return value.strip().lower() in ("true", "yes", "1", "on")
    if isinstance(value, (int, float)):
        return value != 0
    return False


def _issue(level: str, message: str, node: dict | None = None) -> dict:
    item = {"level": level, "message": message, "rule": "migration"}
    if node is not None:
        item["node"] = str(node.get("id", ""))
        item["name"] = str(node.get("name", ""))
    return item


def _migrate_paints(paints: Any) -> None:
    if not isinstance(paints, list):
        return
    for paint in paints:
        if isinstance(paint, dict) and "token" in paint:
            token = paint.pop("token")
            paint.setdefault("colorToken", token)


def _migrate_font(font: Any) -> None:
    if isinstance(font, dict) and "style" in font:
        weight, italic = font_weight(font["style"])
        font.setdefault("weight", weight)
        font.setdefault("italic", italic)


def _migrate_node(node: dict, report: list | None) -> None:
    _migrate_paints(node.get("fills"))
    states = node.get("states")
    if isinstance(states, dict):
        for visual in states.values():
            if isinstance(visual, dict):
                _migrate_paints(visual.get("fills"))

    text = node.get("text")
    if isinstance(text, dict):
        _migrate_font(text.get("font"))
        for run in text.get("runs") or []:
            if isinstance(run, dict):
                _migrate_font(run.get("font"))

    meta = node.get("meta")
    if isinstance(meta, dict):
        if "role" in meta:
            raw = meta["role"]
            role = map_role(raw)
            if role is None:
                if report is not None and raw not in ("", None):
                    report.append(_issue("warning", f"Unknown v1 meta.role {raw!r} left in meta (importer falls back to the name prefix)", node))
            else:
                del meta["role"]
                node.setdefault("role", role)
                # UE v1 behaviour: an explicit meta.role makes a variable unless it is a spacer
                node.setdefault("variable", role not in ("spacer", "ignore", "auto"))
        if "variable" in meta:
            raw = meta.pop("variable")
            if truthy(raw):
                node["variable"] = True
        if not meta:
            del node["meta"]

    children = node.get("children")
    if isinstance(children, list):
        for child in children:
            if isinstance(child, dict):
                _migrate_node(child, report)


def migrate(doc: dict, report: list | None = None) -> dict:
    """Return a dwocanvas/2 copy of ``doc``. Migration notes (lint-style dicts) are appended to ``report``.

    - schema -> dwocanvas/2; images -> assets (file -> uri)
    - capabilities = EXTRACT_CAPABILITIES, source.mode = "extract" (source.exporter is copied to source.adapter)
    - meta.role (UE enum names) -> role, meta.variable -> variable; paint token -> colorToken
    - text font style -> weight / italic (style kept)
    - unknown top-level keys -> "x-<key>"
    """
    if not isinstance(doc, dict):
        raise TypeError("document must be a JSON object")
    out = copy.deepcopy(doc)
    if out.get("schema") == SCHEMA_V2:
        return out
    if out.get("schema") not in (SCHEMA_V1, None) and report is not None:
        report.append(_issue("warning", f"Unknown schema {out.get('schema')!r}: migrated as {SCHEMA_V1}"))

    result: dict[str, Any] = {"schema": SCHEMA_V2}

    source = out.get("source")
    source = dict(source) if isinstance(source, dict) else {}
    if "exporter" in source and "adapter" not in source:
        source["adapter"] = source["exporter"]
    source.setdefault("mode", "extract")
    result["source"] = source
    result["capabilities"] = list(EXTRACT_CAPABILITIES)

    images = out.get("images")
    if isinstance(images, dict):
        assets = {}
        for asset_id, info in images.items():
            if isinstance(info, dict):
                info = dict(info)
                if "file" in info:
                    info.setdefault("uri", info.pop("file"))
            assets[asset_id] = info
        result["assets"] = assets
    elif isinstance(out.get("assets"), dict):
        result["assets"] = out["assets"]

    for key in ("documents", "components"):
        if key in out:
            result[key] = out[key]
            for document in out[key] if isinstance(out[key], list) else []:
                if isinstance(document, dict) and isinstance(document.get("root"), dict):
                    _migrate_node(document["root"], report)

    for key in ("lint", "usage", "tokens"):
        if key in out:
            result[key] = out[key]

    for key, value in out.items():
        if key in V2_TOP_LEVEL or key == "images" or key in result:
            continue
        result[key if key.startswith("x-") else "x-" + key] = value

    if report is not None:
        report.append(_issue("info", f"Migrated from {SCHEMA_V1} to {SCHEMA_V2}"))
    return result

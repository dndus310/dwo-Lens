"""Shared test helpers: puts canvas/python on sys.path and builds small v2 fixtures."""

import copy
import json
import sys
from pathlib import Path

PYTHON_DIR = Path(__file__).resolve().parents[1]
CANVAS_DIR = PYTHON_DIR.parent
REPO_ROOT = CANVAS_DIR.parent
SAMPLES_DIR = REPO_ROOT / "plugin" / "DwoCanvas" / "Samples"
FIGMA_DIR = CANVAS_DIR / "adapters" / "figma"

if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def node(id_, name, type_="frame", **extra):
    n = {"id": id_, "name": name, "type": type_, "box": {"x": 0, "y": 0, "w": 10, "h": 10}}
    n.update(extra)
    return n


BASE_CAPS = ["layout.flow", "layout.constraints", "paint.gradient", "paint.image", "effects", "components",
             "variants", "preview", "stable-ids"]


def v2_doc(root=None, components=None, assets=None, capabilities=None, source=None, preview=None):
    """A minimal valid dwocanvas/2 document around ``root``."""
    document = {"id": "D:1", "name": "Screen", "kind": "screen", "root": root or node("1:1", "Screen")}
    if preview is not None:
        document["preview"] = preview
    doc = {
        "schema": "dwocanvas/2",
        "source": source if source is not None else {"tool": "hand", "adapter": "test/1", "mode": "extract"},
        "capabilities": list(BASE_CAPS) if capabilities is None else capabilities,
        "assets": assets if assets is not None else {},
        "documents": [document],
    }
    if components is not None:
        doc["components"] = components
    return copy.deepcopy(doc)

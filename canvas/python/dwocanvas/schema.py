"""Minimal JSON Schema (draft 2020-12) validator for the DwoCanvas schema files.

Standard library only. Supports the keyword subset the DwoCanvas schemas use:
local ``$ref`` / ``$defs``, ``type`` (string or list, ``integer``), ``enum``, ``const``, ``required``,
``properties``, ``additionalProperties`` (bool or schema), ``patternProperties``, ``items``,
``minItems``, ``maxItems``, ``uniqueItems``, ``minimum``, ``maximum``, ``minLength``, ``pattern``, ``anyOf``.
Annotation keywords (``title``, ``description``, ``default``, ``$id``, ``$schema``) are ignored.
"""

from __future__ import annotations

import json
import os
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator

SCHEMA_V2 = "dwocanvas/2"
SCHEMA_V1 = "designbridge/1"

#: Schema file per version id.
SCHEMA_FILES = {
    SCHEMA_V2: "dwocanvas.schema.json",
    SCHEMA_V1: "designbridge-1.schema.json",
}

#: canvas/python/dwocanvas/schema.py -> repo root is three levels above the package directory.
REPO_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_SCHEMA_DIR = REPO_ROOT / "plugin" / "DwoCanvas" / "Schema"
#: Environment variable that overrides the schema directory.
SCHEMA_DIR_ENV = "DWOCANVAS_SCHEMA_DIR"


def schema_dir(override: str | os.PathLike | None = None) -> Path:
    """Directory holding the schema files: explicit override, then $DWOCANVAS_SCHEMA_DIR, then the repo default."""
    if override:
        return Path(override)
    env = os.environ.get(SCHEMA_DIR_ENV)
    return Path(env) if env else DEFAULT_SCHEMA_DIR


_CACHE: dict[tuple[str, str], dict] = {}


def load_schema(version: str = SCHEMA_V2, directory: str | os.PathLike | None = None) -> dict:
    """Load the schema for ``version`` ("dwocanvas/2" or "designbridge/1") or from a file path."""
    if version in SCHEMA_FILES:
        path = schema_dir(directory) / SCHEMA_FILES[version]
    else:
        path = Path(version)
    key = (str(path.resolve()), version)
    if key not in _CACHE:
        with open(path, encoding="utf-8") as f:
            _CACHE[key] = json.load(f)
    return _CACHE[key]


@dataclass
class SchemaError:
    path: str  # JSON pointer into the instance ("" = root)
    message: str
    keyword: str

    def __str__(self) -> str:
        return f"{self.path or '/'}: {self.message}"


def pointer_escape(token: Any) -> str:
    return str(token).replace("~", "~0").replace("/", "~1")


def pointer_join(path: str, token: Any) -> str:
    return path + "/" + pointer_escape(token)


def pointer_tokens(pointer: str) -> list[str]:
    if not pointer:
        return []
    return [t.replace("~1", "/").replace("~0", "~") for t in pointer.lstrip("/").split("/")]


def resolve_pointer(document: Any, pointer: str) -> Any:
    """Resolve a JSON pointer ("/a/0/b") inside ``document``. Raises KeyError / IndexError."""
    node = document
    for token in pointer_tokens(pointer):
        if isinstance(node, list):
            node = node[int(token)]
        else:
            node = node[token]
    return node


def _type_ok(value: Any, type_name: str) -> bool:
    if type_name == "object":
        return isinstance(value, dict)
    if type_name == "array":
        return isinstance(value, list)
    if type_name == "string":
        return isinstance(value, str)
    if type_name == "boolean":
        return isinstance(value, bool)
    if type_name == "null":
        return value is None
    if type_name == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if type_name == "integer":
        if isinstance(value, bool):
            return False
        return isinstance(value, int) or (isinstance(value, float) and value.is_integer())
    return False


def json_equal(a: Any, b: Any) -> bool:
    """JSON equality (True != 1, 1 == 1.0)."""
    if isinstance(a, bool) or isinstance(b, bool):
        return isinstance(a, bool) and isinstance(b, bool) and a == b
    if isinstance(a, (int, float)) and isinstance(b, (int, float)):
        return a == b
    if isinstance(a, list) and isinstance(b, list):
        return len(a) == len(b) and all(json_equal(x, y) for x, y in zip(a, b))
    if isinstance(a, dict) and isinstance(b, dict):
        return a.keys() == b.keys() and all(json_equal(a[k], b[k]) for k in a)
    return type(a) is type(b) and a == b


def _short(value: Any, limit: int = 60) -> str:
    text = json.dumps(value, ensure_ascii=False)
    return text if len(text) <= limit else text[: limit - 3] + "..."


class Validator:
    """Validates instances against one root schema (``$ref`` resolves inside that root)."""

    def __init__(self, schema: dict):
        self.root = schema
        self._patterns: dict[str, re.Pattern] = {}

    def is_valid(self, instance: Any) -> bool:
        return next(self.iter_errors(instance), None) is None

    def errors(self, instance: Any) -> list[SchemaError]:
        return list(self.iter_errors(instance))

    def iter_errors(self, instance: Any, schema: Any = None, path: str = "") -> Iterator[SchemaError]:
        if schema is None:
            schema = self.root
        if schema is True or schema == {}:
            return
        if schema is False:
            yield SchemaError(path, "no value is allowed here", "false")
            return

        if "$ref" in schema:
            yield from self.iter_errors(instance, self._resolve_ref(schema["$ref"]), path)

        if "type" in schema:
            types = schema["type"] if isinstance(schema["type"], list) else [schema["type"]]
            if not any(_type_ok(instance, t) for t in types):
                yield SchemaError(path, f"expected {' or '.join(types)}, got {_short(instance)}", "type")
                return  # further keywords would only add noise

        if "const" in schema and not json_equal(instance, schema["const"]):
            yield SchemaError(path, f"expected {_short(schema['const'])}, got {_short(instance)}", "const")

        if "enum" in schema and not any(json_equal(instance, e) for e in schema["enum"]):
            yield SchemaError(path, f"{_short(instance)} is not one of {_short(schema['enum'], 200)}", "enum")

        if "anyOf" in schema:
            branches = schema["anyOf"]
            if not any(next(self.iter_errors(instance, b, path), None) is None for b in branches):
                details = "; ".join(
                    str(next(self.iter_errors(instance, b, path)).message) for b in branches
                )
                yield SchemaError(path, f"{_short(instance)} matches none of the allowed forms ({details})", "anyOf")

        if isinstance(instance, dict):
            yield from self._object(instance, schema, path)
        elif isinstance(instance, list):
            yield from self._array(instance, schema, path)
        elif isinstance(instance, str):
            if "minLength" in schema and len(instance) < schema["minLength"]:
                yield SchemaError(path, f"string shorter than {schema['minLength']}", "minLength")
            if "pattern" in schema and not self._pattern(schema["pattern"]).search(instance):
                yield SchemaError(path, f"{_short(instance)} does not match pattern {schema['pattern']}", "pattern")
        elif _type_ok(instance, "number"):
            if "minimum" in schema and instance < schema["minimum"]:
                yield SchemaError(path, f"{instance} is less than minimum {schema['minimum']}", "minimum")
            if "maximum" in schema and instance > schema["maximum"]:
                yield SchemaError(path, f"{instance} is greater than maximum {schema['maximum']}", "maximum")

    # -----------------------------------------------------------------------------------------------------------------

    def _object(self, instance: dict, schema: dict, path: str) -> Iterator[SchemaError]:
        for key in schema.get("required", []):
            if key not in instance:
                yield SchemaError(path, f"missing required property '{key}'", "required")
        properties = schema.get("properties", {})
        patterns = schema.get("patternProperties", {})
        additional = schema.get("additionalProperties", True)
        for key, value in instance.items():
            child = pointer_join(path, key)
            matched = False
            if key in properties:
                matched = True
                yield from self.iter_errors(value, properties[key], child)
            for pattern, sub in patterns.items():
                if self._pattern(pattern).search(key):
                    matched = True
                    yield from self.iter_errors(value, sub, child)
            if not matched and additional is not True:
                if additional is False:
                    yield SchemaError(child, f"unknown property '{key}'", "additionalProperties")
                else:
                    yield from self.iter_errors(value, additional, child)

    def _array(self, instance: list, schema: dict, path: str) -> Iterator[SchemaError]:
        if "minItems" in schema and len(instance) < schema["minItems"]:
            yield SchemaError(path, f"expected at least {schema['minItems']} items, got {len(instance)}", "minItems")
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            yield SchemaError(path, f"expected at most {schema['maxItems']} items, got {len(instance)}", "maxItems")
        if schema.get("uniqueItems"):
            for i in range(len(instance)):
                if any(json_equal(instance[i], instance[j]) for j in range(i)):
                    yield SchemaError(pointer_join(path, i), f"duplicate item {_short(instance[i])}", "uniqueItems")
        if "items" in schema:
            for i, item in enumerate(instance):
                yield from self.iter_errors(item, schema["items"], pointer_join(path, i))

    def _resolve_ref(self, ref: str) -> Any:
        if not ref.startswith("#"):
            raise ValueError(f"only local $ref is supported: {ref}")
        return resolve_pointer(self.root, ref[1:])

    def _pattern(self, pattern: str) -> re.Pattern:
        if pattern not in self._patterns:
            self._patterns[pattern] = re.compile(pattern)
        return self._patterns[pattern]


def validate_schema(instance: Any, version: str = SCHEMA_V2, directory: str | os.PathLike | None = None) -> list[SchemaError]:
    """Schema errors of ``instance`` against the named schema version (empty list = valid)."""
    return Validator(load_schema(version, directory)).errors(instance)

"""Schema + rules validation of a DwoCanvas document (v1 input is migrated first)."""

from __future__ import annotations

import os
from typing import Any

from . import migrate as migrate_module
from . import rules as rules_module
from .schema import SCHEMA_V1, SCHEMA_V2, SchemaError, Validator, load_schema, pointer_tokens

LEVELS = ("error", "warning", "info")


def _node_at(doc: Any, pointer: str) -> dict | None:
    """Innermost node object (has id/type and sits under documents/components) on the path of a JSON pointer."""
    found = None
    current = doc
    tokens = pointer_tokens(pointer)
    for i, token in enumerate(tokens):
        try:
            current = current[int(token)] if isinstance(current, list) else current[token]
        except (KeyError, IndexError, ValueError, TypeError):
            break
        # node objects are /documents/N/root(/children/M)* (or /components/...)
        if (isinstance(current, dict) and i >= 2 and tokens[0] in ("documents", "components") and tokens[2] == "root"
                and (i == 2 or tokens[i - 1] == "children")):
            found = current
    return found


def schema_issue(error: SchemaError, doc: Any) -> dict:
    node = _node_at(doc, error.path)
    return {
        "level": "error",
        "node": str(node.get("id", "")) if node else "",
        "name": str(node.get("name", "")) if node else "",
        "message": f"{error.path or '/'}: {error.message}",
        "rule": "schema",
        "path": error.path,
    }


def validate_document(doc: Any, schema_dir: str | os.PathLike | None = None, run_rules: bool = True) -> tuple[list[dict], Any]:
    """Validate ``doc``. Returns ``(issues, v2_doc)``; v2_doc is the migrated document (or ``doc`` itself if v2).

    Each issue: ``{"level": "error"|"warning"|"info", "node": str, "name": str, "message": str, "rule": str}``
    plus ``"path"`` (JSON pointer into v2_doc) for schema errors.
    """
    issues: list[dict] = []
    if not isinstance(doc, dict):
        issues.append({"level": "error", "node": "", "name": "", "message": "/: document must be a JSON object", "rule": "schema", "path": ""})
        return issues, doc

    version = doc.get("schema")
    if version != SCHEMA_V2:
        notes: list[dict] = []
        doc = migrate_module.migrate(doc, notes)
        for note in notes:
            issues.append({"level": note["level"], "node": note.get("node", ""), "name": note.get("name", ""),
                           "message": note["message"], "rule": note.get("rule", "migration")})
        if version != SCHEMA_V1:
            issues.append({"level": "error", "node": "", "name": "", "message": f"Unknown schema {version!r} (expected {SCHEMA_V2} or {SCHEMA_V1})", "rule": "schema", "path": "/schema"})

    validator = Validator(load_schema(SCHEMA_V2, schema_dir))
    issues.extend(schema_issue(e, doc) for e in validator.iter_errors(doc))
    if run_rules:
        issues.extend(rules_module.check(doc))
    return issues, doc


def validate(doc: Any, schema_dir: str | os.PathLike | None = None) -> list[dict]:
    """Lint items for ``doc`` (schema errors as level "error", then rule items). v1 input is migrated first."""
    return validate_document(doc, schema_dir)[0]


def count_levels(issues: list[dict]) -> dict[str, int]:
    counts = {level: 0 for level in LEVELS}
    for issue in issues:
        counts[issue.get("level", "info")] = counts.get(issue.get("level", "info"), 0) + 1
    return counts


def has_errors(issues: list[dict]) -> bool:
    return any(i.get("level") == "error" for i in issues)

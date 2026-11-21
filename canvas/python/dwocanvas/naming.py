"""Team naming rules (layer-name prefixes -> role / variable / widgetName), as data.

The default rules live in ``naming.json`` next to this module. Adapters call :func:`apply_naming` so the
UE importer receives explicit ``role`` / ``variable`` / ``widgetName`` fields instead of guessing from names.
The Figma adapter embeds an identical copy of ``naming.json`` (checked by a unit test).
"""

from __future__ import annotations

import copy
import json
import os
import re
from pathlib import Path
from typing import Any, Iterator

DEFAULT_RULES_PATH = Path(__file__).with_name("naming.json")

#: Roles that never become Blueprint variables when the role is given explicitly (not from a prefix).
NON_VARIABLE_ROLES = frozenset({"auto", "spacer", "ignore"})

_default_rules: dict | None = None


def load_rules(path: str | os.PathLike | None = None) -> dict:
    """Load naming rules from ``path`` (default: the bundled naming.json). Raises ValueError on a bad shape."""
    global _default_rules
    if path is None and _default_rules is not None:
        return copy.deepcopy(_default_rules)
    with open(path or DEFAULT_RULES_PATH, encoding="utf-8") as f:
        rules = json.load(f)
    check_rules(rules)
    if path is None:
        _default_rules = copy.deepcopy(rules)
    return rules


def default_rules() -> dict:
    return load_rules(None)


def check_rules(rules: Any) -> None:
    if not isinstance(rules, dict) or not isinstance(rules.get("prefixes"), list):
        raise ValueError("naming rules must be an object with a 'prefixes' array")
    for i, rule in enumerate(rules["prefixes"]):
        if not isinstance(rule, dict) or not isinstance(rule.get("prefix"), str) or not rule["prefix"]:
            raise ValueError(f"naming rule {i}: 'prefix' must be a non-empty string")
        if not isinstance(rule.get("role"), str) or not rule["role"]:
            raise ValueError(f"naming rule {i}: 'role' must be a non-empty string")
        if "variable" in rule and not isinstance(rule["variable"], bool):
            raise ValueError(f"naming rule {i}: 'variable' must be a boolean")
    if "variablePrefix" in rules and not isinstance(rules["variablePrefix"], str):
        raise ValueError("'variablePrefix' must be a string")


def sanitize_widget_name(text: Any) -> str:
    """UE-safe identifier: non [A-Za-z0-9_] -> '_', collapse '_', trim '_', 'W_' before a digit, 'Widget' if empty."""
    name = re.sub(r"[^A-Za-z0-9_]", "_", str(text if text is not None else ""))
    name = re.sub(r"_+", "_", name).strip("_")
    if not name:
        return "Widget"
    if name[0].isdigit():
        return "W_" + name
    return name


def match_prefix(name: str, rules: dict | None = None) -> dict | None:
    """The rule with the longest prefix that ``name`` starts with (case-sensitive), or None."""
    rules = rules if rules is not None else default_rules()
    best = None
    for rule in rules["prefixes"]:
        if name.startswith(rule["prefix"]) and (best is None or len(rule["prefix"]) > len(best["prefix"])):
            best = rule
    return best


def resolve(name: str, rules: dict | None = None) -> dict:
    """Naming result for a layer name: ``{"role": str | None, "variable": bool, "widgetName": str, "dollar": bool}``.

    Mirrors the UE fallback: role prefix first (stripped), then the variable prefix on the rest.
    """
    rules = rules if rules is not None else default_rules()
    rest = str(name or "")
    role = None
    variable = False
    rule = match_prefix(rest, rules)
    if rule:
        role = rule["role"]
        variable = bool(rule.get("variable", role not in NON_VARIABLE_ROLES))
        rest = rest[len(rule["prefix"]):]
    var_prefix = rules.get("variablePrefix", "$")
    dollar = bool(var_prefix) and rest.startswith(var_prefix)
    if dollar:
        rest = rest[len(var_prefix):]
        variable = True
    return {"role": role, "variable": variable, "widgetName": sanitize_widget_name(rest), "dollar": dollar}


def iter_nodes(doc: dict) -> Iterator[dict]:
    """Every node of every document and component (depth first). ``states`` visuals are not nodes."""
    for key in ("documents", "components"):
        for document in doc.get(key) or []:
            if isinstance(document, dict) and isinstance(document.get("root"), dict):
                stack = [document["root"]]
                while stack:
                    node = stack.pop()
                    yield node
                    children = node.get("children")
                    if isinstance(children, list):
                        stack.extend(reversed([c for c in children if isinstance(c, dict)]))


def apply_naming_to_node(node: dict, rules: dict | None = None) -> dict:
    """Fill ``role`` / ``variable`` / ``widgetName`` on one node where absent. Returns the node."""
    result = resolve(node.get("name", ""), rules)
    explicit_role = node.get("role")
    if explicit_role is None and result["role"]:
        node["role"] = result["role"]
    if "variable" not in node:
        if explicit_role is not None:
            node["variable"] = result["dollar"] or explicit_role not in NON_VARIABLE_ROLES
        else:
            node["variable"] = result["variable"]
    if "widgetName" not in node:
        node["widgetName"] = result["widgetName"]
    return node


def apply_naming(doc: dict, rules: dict | None = None) -> dict:
    """Fill ``role`` / ``variable`` / ``widgetName`` on every node that does not already have them (in place).

    Nodes without a matching prefix get no ``role`` (the importer treats that as auto).
    Returns ``doc`` for chaining.
    """
    rules = rules if rules is not None else default_rules()
    for node in iter_nodes(doc):
        apply_naming_to_node(node, rules)
    return doc

"""Lint rules beyond the JSON schema (SchemaFirstArchitecture.md section 8). Tool-neutral.

:func:`check` takes a dwocanvas/2 document and returns lint items
``{"level", "node", "name", "message", "rule"}`` ("node" / "name" are "" for document-level items).
"""

from __future__ import annotations

from typing import Any, Iterator

from . import naming

GRADIENT_TYPES = frozenset({"linear", "radial", "angular", "diamond"})

# Rule ids (the "rule" field of every item)
NODE_ID_MISSING = "node-id-missing"
NODE_ID_DUPLICATE = "node-id-duplicate"
DOCUMENT_ID_DUPLICATE = "document-id-duplicate"
WIDGET_NAME_DUPLICATE = "widget-name-duplicate"
ASSET_MISSING = "asset-missing"
COMPONENT_MISSING = "component-missing"
LIST_ENTRY = "list-entry"
CAPABILITY = "capability"
CAPABILITIES_UNDECLARED = "capabilities-undeclared"
DRAFT_NOT_INFERRED = "draft-without-inferred"
INFERRED_NOT_DRAFT = "inferred-without-draft"
EXTENSION_ROLE = "extension-role"
STABLE_IDS = "stable-ids"

ALL_RULES = (
    NODE_ID_MISSING, NODE_ID_DUPLICATE, DOCUMENT_ID_DUPLICATE, WIDGET_NAME_DUPLICATE, ASSET_MISSING,
    COMPONENT_MISSING, LIST_ENTRY, CAPABILITY, CAPABILITIES_UNDECLARED, DRAFT_NOT_INFERRED,
    INFERRED_NOT_DRAFT, EXTENSION_ROLE, STABLE_IDS,
)


def item(level: str, rule: str, message: str, node: dict | None = None) -> dict:
    return {
        "level": level,
        "node": str(node.get("id", "")) if isinstance(node, dict) else "",
        "name": str(node.get("name", "")) if isinstance(node, dict) else "",
        "message": message,
        "rule": rule,
    }


def effective_role(node: dict) -> str | None:
    """``role`` if set, else the importer's fallback: the default naming prefix."""
    role = node.get("role")
    if isinstance(role, str) and role:
        return role
    rule = naming.match_prefix(str(node.get("name", "")), naming.default_rules())
    return rule["role"] if rule else None


def effective_widget_name(node: dict) -> str:
    """``widgetName`` if set, else the sanitized name with the default prefixes stripped."""
    name = node.get("widgetName")
    if isinstance(name, str) and name:
        return name
    return naming.resolve(str(node.get("name", "")), naming.default_rules())["widgetName"]


def effective_variable(node: dict) -> bool:
    """``variable`` if set, else the naming fallback ($ prefix or a variable role prefix)."""
    if isinstance(node.get("variable"), bool):
        return node["variable"]
    return naming.resolve(str(node.get("name", "")), naming.default_rules())["variable"]


def _documents(doc: dict) -> Iterator[tuple[str, dict]]:
    for key in ("documents", "components"):
        documents = doc.get(key)
        if isinstance(documents, list):
            for document in documents:
                if isinstance(document, dict):
                    yield key, document


def _walk(node: Any, parent: dict | None = None) -> Iterator[tuple[dict, dict | None]]:
    if not isinstance(node, dict):
        return
    yield node, parent
    children = node.get("children")
    if isinstance(children, list):
        for child in children:
            yield from _walk(child, node)


def _paints(node: dict) -> Iterator[dict]:
    """Fills of the node and of its button state visuals."""
    lists = [node.get("fills")]
    states = node.get("states")
    if isinstance(states, dict):
        lists += [v.get("fills") for v in states.values() if isinstance(v, dict)]
    for paints in lists:
        if isinstance(paints, list):
            yield from (p for p in paints if isinstance(p, dict))


def _bakes(node: dict) -> Iterator[dict]:
    if isinstance(node.get("bake"), dict):
        yield node["bake"]
    states = node.get("states")
    if isinstance(states, dict):
        for visual in states.values():
            if isinstance(visual, dict) and isinstance(visual.get("bake"), dict):
                yield visual["bake"]


def check(doc: dict) -> list[dict]:
    """All lint items for a dwocanvas/2 document."""
    if not isinstance(doc, dict):
        return []
    out: list[dict] = []
    assets = doc.get("assets") if isinstance(doc.get("assets"), dict) else {}
    capabilities = doc.get("capabilities")
    declared = isinstance(capabilities, list)
    caps = set(c for c in capabilities if isinstance(c, str)) if declared else set()
    source = doc.get("source") if isinstance(doc.get("source"), dict) else {}

    document_ids: dict[str, str] = {}
    component_ids = set()
    for key, document in _documents(doc):
        doc_id = document.get("id")
        if isinstance(doc_id, str) and doc_id:
            if doc_id in document_ids:
                out.append(item("error", DOCUMENT_ID_DUPLICATE, f"Document id '{doc_id}' is used by more than one document / component", None))
            document_ids[doc_id] = key
            if key == "components":
                component_ids.add(doc_id)

    usage = {"gradient": [], "image": [], "effects": [], "inferred": []}

    for key, document in _documents(doc):
        preview = document.get("preview")
        if isinstance(preview, str) and preview not in assets:
            out.append(item("error", ASSET_MISSING, f"Document '{document.get('name', '')}' preview asset '{preview}' is not in assets", None))

        seen_ids: dict[str, dict] = {}
        for node, parent in _walk(document.get("root")):
            node_id = node.get("id")
            if not isinstance(node_id, str) or not node_id:
                out.append(item("error", NODE_ID_MISSING, "Node has no id (re-import identity)", node))
            elif node_id in seen_ids:
                out.append(item("error", NODE_ID_DUPLICATE, f"Node id '{node_id}' appears more than once in document '{document.get('name', '')}'", node))
            else:
                seen_ids[node_id] = node

            # Assets
            for paint in _paints(node):
                ptype = paint.get("type")
                if ptype in GRADIENT_TYPES:
                    usage["gradient"].append(node)
                elif ptype == "image":
                    usage["image"].append(node)
                    image = paint.get("image")
                    if not isinstance(image, str) or not image:
                        out.append(item("warning", ASSET_MISSING, "Image paint has no asset id (drawn empty)", node))
                    elif image not in assets:
                        out.append(item("error", ASSET_MISSING, f"Image paint asset '{image}' is not in assets", node))
            for bake in _bakes(node):
                image = bake.get("image")
                if isinstance(image, str) and image not in assets:
                    out.append(item("error", ASSET_MISSING, f"Baked image asset '{image}' is not in assets", node))
            if isinstance(node.get("effects"), list) and node["effects"]:
                usage["effects"].append(node)
            if "confidence" in node or "inferred" in node:
                usage["inferred"].append(node)

            # Components
            component = node.get("component")
            if isinstance(component, dict):
                ref = component.get("ref")
                # Buttons are built from their own visuals + states: the Figma adapter does not export their components
                if isinstance(ref, str) and ref and ref not in component_ids and effective_role(node) != "button":
                    out.append(item("warning", COMPONENT_MISSING, f"Component '{ref}' ({component.get('name', '?')}) is not in components: the instance is built inline", node))

            role = effective_role(node)
            if isinstance(role, str) and role.startswith("x-"):
                out.append(item("info", EXTENSION_ROLE, f"Role '{role}' needs a registered role handler (otherwise built as a plain container)", node))

            children = node.get("children")
            if role in ("list", "tile"):
                first = children[0] if isinstance(children, list) and children else None
                ok = (isinstance(first, dict) and first.get("type") == "instance"
                      and isinstance(first.get("component"), dict) and first["component"].get("ref"))
                if not ok:
                    out.append(item("warning", LIST_ENTRY, f"{role} needs a component instance as its first child (the entry widget class); set it by hand otherwise", node))

            # Sibling widget names (list / tile children are sample entries: repeats expected)
            if isinstance(children, list) and role not in ("list", "tile"):
                names: dict[str, dict] = {}
                for child in children:
                    if not isinstance(child, dict) or effective_role(child) == "ignore":
                        continue
                    widget = effective_widget_name(child)
                    if widget in names:
                        # A suffix only breaks something (BindWidget, Blueprint references) when one of them is a variable
                        level = "warning" if effective_variable(child) or effective_variable(names[widget]) else "info"
                        out.append(item(level, WIDGET_NAME_DUPLICATE, f"Widget name '{widget}' is used by another child of '{node.get('name', '')}': the importer adds a suffix", child))
                    else:
                        names[widget] = child

    # Capabilities
    if not declared:
        out.append(item("info", CAPABILITIES_UNDECLARED, "No capabilities declared: capability checks skipped", None))
    else:
        for cap, key, what in (("paint.gradient", "gradient", "gradient paints"), ("paint.image", "image", "image paints"),
                               ("effects", "effects", "effects"), ("inferred", "inferred", "confidence / inferred fields")):
            nodes = usage[key]
            if nodes and cap not in caps:
                out.append(item("warning", CAPABILITY, f"{len(nodes)} node(s) use {what} but capabilities lacks '{cap}'", nodes[0]))

    mode = source.get("mode")
    if mode == "draft" and "inferred" not in caps:
        out.append(item("error", DRAFT_NOT_INFERRED, "source.mode is 'draft' but capabilities lacks 'inferred'", None))
    if "inferred" in caps and mode != "draft":
        out.append(item("warning", INFERRED_NOT_DRAFT, "capabilities has 'inferred' but source.mode is not 'draft'", None))
    if "stable-ids" not in caps:
        out.append(item("info", STABLE_IDS, "No 'stable-ids' capability: re-import preservation not guaranteed", None))
    return out

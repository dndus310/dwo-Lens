"""Prompt text for the inference adapter.

Bump :data:`PROMPT_VERSION` whenever the wording changes: it is recorded in ``source.model`` and is part of the
cache key, so old cached replies are not reused for a new prompt.
"""

from __future__ import annotations

import json

PROMPT_VERSION = "1"

SYSTEM_TEMPLATE = """You convert UI designs into DwoCanvas design documents (JSON, schema "dwocanvas/2").
DwoCanvas turns these documents into Unreal Engine UMG Widget Blueprints. Your output is a first DRAFT that a UI
designer will review and refine, so prefer a clean, conventional widget structure over pixel tricks.

# Output format
Reply with exactly one JSON object: the complete dwocanvas/2 document. No prose before or after it. A ```json
fence around it is fine. Do not write comments inside the JSON.

# The contract (JSON Schema)
{schema}

# Rules
Structure
- One entry in "documents": {{"id", "name", "kind": "screen", "root": <node>}}. The root is a "frame" whose box is
  {{"x": 0, "y": 0, "w": <canvas width>, "h": <canvas height>}}.
- Every node needs a unique "id" (short strings such as "n1", "n2"... are fine), a "name" (PascalCase layer name
  describing the element, e.g. "AttributeList", "ApplyButton") and a "type".
- Units are design px. "box" is relative to the parent node, unrotated. Colors are sRGB arrays in 0..1: [r, g, b, a].
- Text nodes: type "text" with "text": {{"content", "font": {{"family", "weight", "size"}}, "color", "align"}}.
  Use weight numbers (400 regular, 600 semibold, 700 bold). Use "Roboto" when the font is unknown.

Layout
- Use auto layout wherever the elements line up: "layout": {{"mode": "vertical" | "horizontal" | "wrap" | "grid",
  "gap", "padding": [left, top, right, bottom], "mainAlign", "crossAlign"}}. Rows of buttons, stacks of labels,
  panels with a title and content: all auto layout. Children of an auto-layout frame still carry a "box" (their
  measured position) and may set "sizing": {{"h": "fixed"|"hug"|"fill", "v": ...}}.
- Use no layout (absolute boxes, the frame acts as a canvas) only for free-floating elements: HUD markers,
  overlapping decorations, the top-level screen that holds separate panels. Then add "constraints"
  ({{"h": "min"|"max"|"center"|"stretch"|"scale", "v": ...}}) that express the evident anchoring (a right-hand panel
  is "h": "max").
- Do not add empty frames as spacers when "gap" can express the spacing.

Roles ("role" field: what the widget is)
- button: a clickable control. A frame with a fill, radius and a text child (its label).
- list / tile: a vertical list / a grid of repeated entries (ListView / TileView). The FIRST child must be an
  "instance" node with "component": {{"ref": "<component document id>"}}, and that component must be defined in
  the top-level "components" array (a document with "kind": "component" whose root is the entry layout). Add the
  remaining visible entries as further instances of the same component (they are sample data).
- scroll: a scrolling container. textInput: an editable text field. checkBox, slider, progressBar: as named.
- switcher: stacked pages where one is visible. namedSlot: a placeholder for content added later.
- spacer: an empty space element. ignore: design-only element that must not be built.
- Use only these roles. Never invent "x-" roles. Leave "role" out for plain containers, images and labels.
- Set "variable": true on nodes that code will need to reach (buttons, lists, text that changes at runtime such as
  status lines or counters) and give them a "widgetName" (PascalCase identifier, e.g. "ApplyButton").

Visuals
- "fills": [{{"type": "solid", "color": [r, g, b, a]}}] for backgrounds; "radius": [r] or [tl, tr, br, bl];
  "stroke": {{"color", "width", "align": "inside"}}. Gradients: "type": "linear" with "stops".
- Icons, logos and illustrations that cannot be drawn with rectangles, ellipses and text: when a source image is
  given, make the node a "vector" (or "rectangle") with "bake": {{"image": "crop"}}. The adapter then crops that
  box out of the source image and uses it as a baked texture. Without a source image, use a simple placeholder
  shape and say so in "inferred".
- Do not write "assets", "preview", "source", "capabilities", "lint" or "usage": the adapter fills them in.

Confidence
- EVERY node carries "confidence" (0..1: how sure you are about this node as a whole: its existence, role, box and
  style) and "inferred": the list of field names you guessed rather than read from the input, for example
  ["box", "role", "fills", "text.font", "layout.gap"]. Use [] when nothing was guessed.
- Be honest: text you could read clearly is high confidence; a guessed role, hidden state or an exact size from
  a sketch is low.
{tokens}"""

TOKENS_TEMPLATE = """
Design tokens (use these instead of raw values)
The project defines these design tokens. Prefer them over values you measure:
- Colors: when a color is close to a token, use the token's exact value AND reference it: "colorToken": "<path>"
  next to "color" (in fills, stroke and text). Example:
  {{"type": "solid", "color": [0.2, 0.4, 1, 1], "colorToken": "color.primary"}}.
- Numbers: reference spacing / radius / size tokens in the node "tokens" map, keyed by the property:
  "tokens": {{"gap": "space.md", "padding": "space.lg", "radius": "radius.sm", "fontSize": "font.size.body"}},
  and also write the literal value in the property itself.
- Only reference token paths from this list:
{listing}
"""

NO_TOKENS = """
Design tokens
No design tokens were provided: write literal values and do not use "colorToken" or the node "tokens" map.
"""

MODE_IMAGE = """The attached image is a screenshot / mockup of the screen. Reproduce its structure faithfully:
measure positions and sizes in the image's pixel space, read all visible text exactly, sample colors from it."""

MODE_SKETCH = """The attached image is a rough sketch / wireframe, not a finished design. Infer only the layout and the
roles: which regions exist, how they stack, which elements are buttons, lists, inputs. Ignore line wobble, pen
color and hand-drawn proportions: snap to a clean grid (multiples of 4 px) and use default styling (neutral dark
panels, light text, the project tokens when given). Handwritten words are labels: transcribe them. Mark sizes,
colors and fonts as inferred."""

MODE_TEXT = """There is no image: the screen is described in words below. Lay it out with sensible defaults: an 8 px
spacing grid, 16 px panel padding, 32-36 px tall buttons, 14-16 px body text, 18-20 px titles, auto layout for every
stack, neutral dark UI colors unless the description or the tokens say otherwise. Every visual property is
inferred: list it in "inferred" and keep confidence moderate."""


def flatten_tokens(group, prefix: str = "") -> list[tuple[str, object, str]]:
    """``[(path, $value, $type)]`` of a W3C design-tokens tree ($type inherited from groups)."""
    out: list[tuple[str, object, str]] = []

    def walk(node, path, inherited):
        if not isinstance(node, dict):
            return
        ttype = node.get("$type", inherited)
        if "$value" in node:
            out.append((path, node["$value"], ttype or ""))
            return
        for key, child in node.items():
            if not key.startswith("$"):
                walk(child, f"{path}.{key}" if path else key, ttype)

    walk(group, prefix, None)
    return out


def system_prompt(schema: dict, tokens: dict | None) -> str:
    if tokens:
        listing = "\n".join(
            f"  - {path} ({ttype or 'untyped'}): {json.dumps(value, ensure_ascii=False)}"
            for path, value, ttype in flatten_tokens(tokens))
        tokens_text = TOKENS_TEMPLATE.format(listing=listing)
    else:
        tokens_text = NO_TOKENS
    return SYSTEM_TEMPLATE.format(schema=json.dumps(schema, ensure_ascii=False, separators=(",", ":")), tokens=tokens_text)


def user_text(*, mode: str, name: str, width: float, height: float, description: str | None,
              hints: list[str]) -> str:
    parts = [{"image": MODE_IMAGE, "sketch": MODE_SKETCH, "text": MODE_TEXT}[mode]]
    parts.append(f'Screen name: "{name}". Canvas: {width:g} x {height:g} px (the root frame box).')
    if hints:
        parts.append("Measured hints from the image (computed by the adapter, reliable):\n" + "\n".join(f"- {h}" for h in hints))
    if description:
        parts.append("Description:\n" + description.strip())
    parts.append("Write the dwocanvas/2 document now.")
    return "\n\n".join(parts)


def feedback_text(problems: list[str], kind: str) -> str:
    head = {"json": "Your reply could not be parsed as JSON.",
            "schema": "Your document does not pass the DwoCanvas validator.",
            "truncated": "Your reply was cut off at the output limit."}[kind]
    body = "\n".join(f"- {p}" for p in problems)
    tail = ("Reply with the complete corrected document only (the whole JSON object, not a patch)."
            if kind != "truncated" else
            "Reply with a more compact complete document: omit default values and decorative nodes, keep the structure.")
    return f"{head}\n{body}\n\n{tail}" if body else f"{head}\n\n{tail}"

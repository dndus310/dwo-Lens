# DwoCanvas - Python tools (`dwocanvas`)

Tool-neutral side of DwoCanvas: the JSON Schema validator, `designbridge/1` -> `dwocanvas/2` migration,
team naming rules and lint rules (design: `canvas/docs/SchemaFirstArchitecture.md`, sections 4, 5 and 8).
Python 3.10+ (developed on 3.14), **standard library only**.

Schemas are read from `plugin/DwoCanvas/Schema/` (`dwocanvas.schema.json`, `designbridge-1.schema.json`).
Override with `--schema-dir DIR`, the `DWOCANVAS_SCHEMA_DIR` environment variable, or the `schema_dir` argument.

## CLI

Run from `canvas/python` (or put it on `PYTHONPATH`):

```
python -m dwocanvas validate <file> [--json] [--schema-dir DIR]
python -m dwocanvas migrate  <in> <out> [--schema-dir DIR]
python -m dwocanvas naming   <in> <out> [--rules FILE] [--schema-dir DIR]
```

- `validate`: schema errors (level `error`) + lint rules. A `designbridge/1` file is migrated first (info item `migration`).
  - Human output: `<file> (<schema>)`, then `ERROR (n)` / `WARNING (n)` / `INFO (n)` groups with lines
    `  [<rule>] <node id> '<name>': <message>`, then `Summary: E error(s), W warning(s), I info`.
  - `--json`: a JSON array of lint items on stdout (same shape as the document's `lint` array).
- `migrate`: writes the `dwocanvas/2` copy (v2 input is copied unchanged), then prints the validation report.
- `naming`: migrates if needed, fills `role` / `variable` / `widgetName` from the naming rules (default `dwocanvas/naming.json`), writes, prints the report.
- `<out>` may be `-` (stdout; the report then goes to stderr). `<file>` / `<in>` may be `-` (stdin).
- Exit codes: `0` no errors, `1` the (resulting) document has errors, `2` usage or IO error (unreadable file, bad JSON, missing schema, bad rules file).

## Lint item

```json
{ "level": "error|warning|info", "node": "12:34", "name": "btn_Apply", "message": "...", "rule": "asset-missing" }
```

`node` / `name` are `""` for document-level items. Schema errors also carry `path` (JSON pointer into the v2 document)
and the id of the innermost node on that path.

| rule | level | when |
|---|---|---|
| `schema` | error | JSON Schema violation, unknown `schema` value |
| `migration` | info / warning | the file was `designbridge/1`; unknown v1 `meta.role` (kept in `meta`) |
| `node-id-missing` / `node-id-duplicate` | error | node without id / same id twice in one document |
| `document-id-duplicate` | error | two documents / components share an id |
| `widget-name-duplicate` | warning (info if neither sibling is a variable) | same effective widget name among siblings (list / tile children and `ignore` nodes exempt) |
| `asset-missing` | error (warning for an image paint without id) | paint `image`, `bake.image` (also in `states`), document `preview` not in `assets` |
| `component-missing` | warning | `component.ref` not in `components` (button instances exempt: built from their states) |
| `list-entry` | warning | `list` / `tile` whose first child is not an `instance` with `component.ref` |
| `capability` | warning | gradients without `paint.gradient`, image paints without `paint.image`, effects without `effects`, `confidence` / `inferred` without `inferred` |
| `capabilities-undeclared` | info | no `capabilities` array (capability checks skipped) |
| `draft-without-inferred` | error | `source.mode: "draft"` without the `inferred` capability |
| `inferred-without-draft` | warning | `inferred` capability without `source.mode: "draft"` |
| `extension-role` | info | `x-` role: needs a registered role handler |
| `stable-ids` | info | no `stable-ids` capability: re-import preservation not guaranteed |

"Effective" role / widget name = the field if present, else what the UE importer derives from the default name prefixes.

## Python API

```python
from dwocanvas.validate import validate, validate_document, has_errors, count_levels
from dwocanvas.migrate import migrate, font_weight, EXTRACT_CAPABILITIES
from dwocanvas.naming import apply_naming, load_rules, resolve, sanitize_widget_name
from dwocanvas.rules import check
from dwocanvas.schema import Validator, load_schema, validate_schema

validate(doc, schema_dir=None) -> list[dict]                    # lint items; v1 is migrated first
validate_document(doc, schema_dir=None, run_rules=True) -> (list[dict], v2_doc)
migrate(doc, report=None) -> dict                               # new v2 dict; notes appended to report (list)
apply_naming(doc, rules=None) -> doc                            # in place; only fills missing fields
load_rules(path=None) -> dict                                   # default: dwocanvas/naming.json
resolve(name, rules=None) -> {"role", "variable", "widgetName", "dollar"}
check(v2_doc) -> list[dict]                                     # lint rules only
validate_schema(instance, version="dwocanvas/2", directory=None) -> list[SchemaError]   # .path .message .keyword
```

## Naming rules (`dwocanvas/naming.json`)

`prefixes`: `{prefix, role, variable}`; the longest matching prefix wins (case-sensitive) and is stripped from `widgetName`.
`variablePrefix` (`$`) after the role prefix marks a variable. `widgetName` is sanitized to `[A-Za-z_][A-Za-z0-9_]*`
(non-alnum -> `_`, repeated `_` collapsed, trimmed, `W_` before a leading digit, `Widget` if empty).
Nodes without a matching prefix get no `role`. The Figma adapter embeds an identical copy (unit-tested).

## Tests

```
python -m unittest discover -s canvas/python/tests
```

`canvas/tools/check_js.py` is a Node-free sanity check for the Figma adapter's JavaScript (brackets, strings,
templates, regexes, Python-isms, duplicate top-level declarations, `await` outside `async`).

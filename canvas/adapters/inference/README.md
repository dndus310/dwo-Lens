# DwoCanvas inference adapter (`infer2dwocanvas.py`)

Turns a **screenshot / mockup image**, a **text description** or a **rough sketch** into a *draft*
`dwocanvas/2` document using the Claude API. The DwoCanvas UE plugin then builds Widget Blueprints from it like
from any other adapter. Design: `canvas/docs/SchemaFirstArchitecture.md` section 5.1 (also sections 9 and 10).

## What it is, and what it is not

- **A first-generation draft.** The output says so: `source.mode: "draft"`, capability `inferred`, and every node
  carries `confidence` (0..1) and `inferred` (the fields that were guessed). The importer writes drafts to
  `/Game/UI/Drafts` as new blueprints and warns about low-confidence nodes.
- **No re-import guarantee.** Images have no stable ids; running the same input again gives new ids. Do not
  re-import a draft over work you did in UE. The recommended flow: refine the draft in Figma (or by hand in UE),
  then continue with the normal Figma path.
- Roles: only the built-in ones (`button`, `list`, `tile`, `scroll`, `textInput`, `checkBox`, `slider`,
  `progressBar`, `switcher`, `namedSlot`, `spacer`, `ignore`). Extension roles (`x-...`) are set by people.
- Not inferred (defaults or tokens instead): responsive intent (fill / hug / anchors) beyond the obvious, hover and
  pressed states, component boundaries other than repeated list entries, animations.
- Text input has no visual: everything visual is a default. Sketch input gives layout and roles only.

## Usage

Run from anywhere (the script finds `canvas/python` for the validator). Python 3.10+, standard library only plus
`anthropic` for real API calls (`pip install anthropic`).

```
python canvas/adapters/inference/infer2dwocanvas.py
    --image <png|jpg> | --text "<description>" | --text-file <md>   [--sketch]
    -o <out.dwocanvas.json> [--name Screen] [--width 1280 --height 720]
    [--tokens tokens.json] [--snap-distance 0.06] [--no-snap] [--no-crop]
    [--model claude-opus-5-5] [--effort low|medium|high|xhigh|max] [--max-retries 3] [--no-fallback]
    [--mock <responses.json>] [--no-cache] [--cache-dir DIR] [--quiet]
```

| Option | Meaning |
|---|---|
| `--image` | PNG or JPEG (detected by magic bytes). Sent to the model, embedded as the document `preview`. |
| `--sketch` | The image is a wireframe: layout and roles only, default styling, no color hints. |
| `--text` / `--text-file` | Description of the screen (no image). |
| `--name` | Document (WBP) name. |
| `--width` / `--height` | Canvas size. Default: the image size, else 1280 x 720. |
| `--tokens` | W3C design tokens. The model is told to prefer them; literal colors are snapped to them (below). The tokens are written into the document. |
| `--model` / `--effort` | Default `claude-opus-5-5`, effort `high`. |
| `--max-retries` | Validation-loop retries after the first attempt (default 3, so at most 4 calls). |
| `--no-fallback` | Turns off the server-side refusal fallback (below). |
| `--mock` | Canned replies instead of the API (below). |

Exit codes: `0` valid draft written, `1` the draft still has validator errors after the retries (written anyway,
with its lint) or no reply was parsable JSON, `2` usage / IO / API / missing SDK, `3` refusal, `4` every attempt
was truncated at `max_tokens`.

The report lists **low-confidence nodes first** (`REVIEW FIRST`), then the lint grouped by level. The same
items are written into the document's `lint` array.

## Pipeline

1. **Analysis** (standard library only, `pngtools.py`): PNG size and dominant colors, given to the model as
   reliable hints. JPEG: only the size is read; no color hints and no icon crops (no JPEG decoder in the
   standard library; convert to PNG to get them). **OCR**: no engine is installed. `infer2dwocanvas.register_ocr(fn)`
   is the hook: `fn(image_bytes, media_type) -> [{"text", "box": [x, y, w, h]}]`; its results are added to the
   prompt as hints.
2. **Prompt** (`prompt.py`, `PROMPT_VERSION`): the full schema, the rules (roles, auto layout vs absolute boxes,
   px units, `confidence` / `inferred`, tokens), and the project tokens go in the system prompt. The image (if any)
   goes first in the user message, followed by the mode instructions (image / sketch / text), the hints and the
   description.
3. **Call** (streaming, `client.beta.messages.stream(...).get_final_message()`, `max_tokens` 64000,
   `output_config.effort`). No `temperature` / `top_p` / `top_k` / `thinking` (rejected or always on for this
   model). Structured outputs (`output_config.format`) are not used: the recursive schema does not fit strict mode.
   The system prompt carries a `cache_control` breakpoint so retries reuse the cached schema prefix.
4. **Stop reason** first: `refusal` -> report `stop_details.category` / `explanation`, exit 3, no retry.
   `max_tokens` -> the next attempt asks for a more compact document; exit 4 when attempts run out.
5. **Parse**: join the `text` blocks, strip a ```` ```json ```` fence, `json.loads`.
6. **Post-process**: force `schema: "dwocanvas/2"`; set `source` (`tool: inference`,
   `adapter: dwocanvas-inference/0.1.0`, `mode: draft`, `model: "<model>; prompt v<N>"`, plus `x-inference` with
   effort / attempts / cache); capabilities = `inferred`, `layout.flow`, `preview` (image input), `tokens` (with
   `--tokens`), plus `layout.constraints` / `paint.gradient` / `paint.image` / `effects` / `components` when used;
   nodes without `confidence` get 0.5 and `"confidence"` in `inferred`; ids made unique; the image embedded as
   asset `inference_preview` and set as the document `preview` (the importer measures the visual difference
   against it); `bake: {"image": "crop"}` requests are cropped out of the PNG as baked assets; undefined token
   references dropped (warning); token snapping.
7. **Validation loop**: `dwocanvas.validate.validate(doc)`. On errors (or unparsable JSON) the assistant reply and a
   user message listing the errors are appended and the model is asked again, up to `--max-retries`.
8. Write the file and print the lint.

### Token snapping

With `--tokens`, every literal color without a `colorToken` (solid fills, strokes, text, button states) is compared
with every color token (hex `#rgb[a]` / `#rrggbb[aa]`, DTCG `{colorSpace: "srgb", components, alpha}`, `{alias}`
references followed). Distance: **sRGB Euclidean distance over r, g, b in 0..1, alpha ignored**. If the nearest
token is within `--snap-distance` (default **0.06**, about 9/255 on every channel or 15/255 on one: differences you
barely see), the color becomes the token's value (keeping the original alpha) and `colorToken` is set. `--no-snap`
turns it off. ΔE would track perception better; Euclidean sRGB is used because it is simple, deterministic and good
enough at this small threshold.

## Credentials

The script constructs `anthropic.Anthropic()` with no arguments: the SDK resolves `ANTHROPIC_API_KEY`, or a profile
from `ant auth login`. Nothing is stored by the adapter. Without the package the script stops with
`pip install anthropic` (mock mode does not need it).

## Refusal fallback (on by default)

Requests are sent with `betas=["server-side-fallback-2026-07-01"]` and `fallbacks="default"`: if the model's
safety classifiers decline the request, the API re-runs it on a fallback model inside the same call (routed by
refusal category, billed at the fallback model's rates). If `source.x-inference.servedModel` differs from
`--model`, a fallback model answered. **Turn it off with `--no-fallback`**, which drops `betas` / `fallbacks`; a
refusal then ends the run with exit 3. A refusal of the whole chain also exits 3.

## Cost and latency

- One run = 1 to `1 + --max-retries` calls. Each call sends the schema and rules (about 5k tokens, cached after the
  first call), the image (a 1600 x 900 screenshot is roughly 1.5-2k input tokens) and, on retries, the previous
  reply. A screen's document is typically 10-30k output tokens at Claude Opus 5.5 rates ($4 / $20 per million
  input / output tokens): budget roughly $0.25-$0.75 per screen per attempt; check the current price list.
- Latency: a long, high-effort generation streams for one to several minutes per attempt. `--effort medium`
  is faster and cheaper; measure the quality difference on your screens before lowering it.
- The cache (below) makes re-runs with the same inputs free and instant.

## Cache

The validated model reply (raw text, before post-processing) is cached under
`%LOCALAPPDATA%\dwo-Forge\canvas-inference-cache` (`~/.cache/...` elsewhere; override with `--cache-dir` or
`DWOCANVAS_INFERENCE_CACHE`). Key: sha256 over the inputs (image bytes, text, mode, name, canvas size, tokens), the
model, the prompt version, the effort and the fallback setting (and the mock file, in mock mode). Replies that
still fail validation are not cached. `--no-cache` neither reads nor writes it. Post-processing runs again on a
hit, so adapter fixes apply to cached replies.

## External-API data policy

**The image and/or the description are sent to Anthropic's API.** Screenshots of unreleased UI are confidential
material: the team must explicitly allow sending them to an external API (security and contract review,
SchemaFirstArchitecture.md section 10) before anyone runs this without `--mock`. If that is not allowed, keep using
mock mode for development and replace the model call with an in-house model behind the same pipeline.

## Mock mode (no API)

`--mock responses.json` drives the exact same pipeline, retry loop included, with canned replies used in order,
one per call. Each item is a string (the assistant text), or `{"text": "..."}`, or `{"json": <document>}`
(serialized as a fenced JSON reply), with optional `"stop_reason"` (`end_turn` / `max_tokens` / `refusal`) and
`"stop_details": {"category", "explanation"}`. Mock runs are labelled `(mock)` in `source.model`.

## Sample

`samples/` holds a demo built from the DWO Attribute screen:

- `dwo_attribute.png`: the Figma render of the screen (the `preview` asset `img_1` of
  `plugin/DwoCanvas/Samples/DwoMockup.dwocanvas.json`).
- `dwo_attribute.mock.json`: a hand-written, plausible model reply for that image (two panels with auto layout,
  a ListView and a TileView with entry components, buttons, a baked spawn marker).
- `dwo_tokens.json`: a small W3C token set with the screen's palette.
- `dwo_attribute.draft.dwocanvas.json`: the result of

```
cd canvas/adapters/inference
python infer2dwocanvas.py --image samples/dwo_attribute.png --name AttributeScreen --tokens samples/dwo_tokens.json --mock samples/dwo_attribute.mock.json --no-cache -o samples/dwo_attribute.draft.dwocanvas.json
```

It validates without errors and imports into UE as a draft (`/Game/UI/Drafts`, 0 errors). It shows the format;
it does not measure the model's accuracy (the reply was written by hand).

## Tests

```
python -m unittest discover -s canvas/adapters/inference/tests -v
```

No network and no `anthropic` package needed: mock happy path, validation-loop retries (invalid JSON, schema
errors, truncation), image preview / hints / crops, JPEG, token snapping, refusal handling, cache hits, missing SDK,
SDK error mapping, and that the sample reproduces.

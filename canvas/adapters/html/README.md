# DwoCanvas HTML/CSS adapter

Turns an HTML page into a `dwocanvas/2` document that the DwoCanvas UE plugin imports as a Widget Blueprint.
It is the second extracting adapter next to Figma (`SchemaFirstArchitecture.md`, stage 3): the same screen built
from two tools should import to the same widgets.

```
python canvas/adapters/html/html2dwocanvas.py page.html -o page.dwocanvas.json [--width 1280 --height 720] [--name Screen]
```

| Option | Default |
|---|---|
| `--width` / `--height` | `<meta name="dwocanvas:viewport" content="400x300">`, else 1280 x 720 |
| `--name` | `<meta name="dwocanvas:name" content="Shop">`, else the file name |
| `--browser` | `$DWOCANVAS_BROWSER`, then Edge, then Chrome (the first one that produces output) |
| `--rules` | the shared naming rules (`canvas/python/dwocanvas/naming.json`) |
| `--no-preview` | off: the 1x screenshot is embedded as the document `preview` |
| `--raw file.json` | also write the extractor's raw output (debugging) |
| `-v` | print info lint items too (warnings and errors are always printed) |

Exit code: 0 written, 1 validation errors (nothing written), 2 extraction failed. Standard library only (Python 3.10+).

## How it works

1. **Layout in a headless browser.** The page is copied to a temp folder with a `<base href>` pointing at its own
   folder (relative URLs keep working) and `extractor.js` appended. A wrapper page shows it in an iframe of exactly
   `width x height` (the headless window itself is not a reliable viewport size), and Chromium runs with
   `--headless=new --disable-gpu --force-device-scale-factor=1 --disable-lcd-text --dump-dom --virtual-time-budget=...`
   (greyscale text anti-aliasing like Slate; LCD sub-pixel text made the text case 1 point worse).
   A browser that prints no extractor output is skipped: on the development machine Edge 157's headless mode exits
   at once without output, so the adapter falls through to Chrome.
   The extractor waits for web fonts and images, then walks the DOM with `getComputedStyle` /
   `getBoundingClientRect` and posts a raw JSON tree to the wrapper, which `--dump-dom` prints.
2. **Preview.** A second run (`--screenshot`) renders the original page in the same wrapper; the PNG becomes asset
   `preview` and the document `preview` (capability `preview`).
3. **Mapping** (`html2dwocanvas.py`, testable without a browser) and **validation** with `dwocanvas.validate`.

## Mapping

| CSS / HTML | dwocanvas/2 |
|---|---|
| `display: flex` row / column | `layout.mode` horizontal / vertical; `flex-wrap` on a row → `wrap` (+ `crossGap` = row-gap) |
| `gap` / `row-gap` / `column-gap`, padding | `layout.gap`, `layout.padding` (the rendered spacing is measured; margins between items fold into it when even) |
| `justify-content` | `mainAlign` start / center / end / space-between (space-around / -evenly approximated, info) |
| `align-items` | `crossAlign`; `stretch` is kept only when every item is stretched (CSS only stretches auto-sized items) |
| item `flex-grow` > 0 / stretched | `sizing` fill on the main / cross axis |
| item width / height | probed in the browser: if setting it to `auto` changes the box it is `fixed`, else `hug` |
| `display: grid` | `grid` with `columns` = track count, gap = column-gap, crossGap = row-gap; cells are fixed size |
| `position: absolute` in a flex / grid parent | `positioning: absolute` (box relative to the parent's border box) |
| anything else | plain boxes in a `none` layout (canvas) |
| `background-color` | solid paint (sRGB 0..1, alpha in the colour) |
| `linear-` / `radial-` / `conic-gradient` | linear / radial / angular paint, stops + handles (start, end, width in 0..1 of the box) |
| `background-image: url()` / `<img>` | image paint, the file embedded as an asset (base64 + sha256); `cover` → fill, `contain` → fit, `repeat` → tile (+ `scalingFactor`) |
| `border` | stroke, align inside (layouts set `strokesIncluded`, as CSS borders take layout space) |
| `border-*-radius` | `radius` [tl, tr, br, bl] (percentages resolved, overlapping radii scaled like CSS) |
| `box-shadow` | drop-shadow / inner-shadow (offset, blur, spread, colour) |
| `backdrop-filter: blur()` / `filter: blur()` | background-blur / layer-blur |
| `opacity`, `transform: rotate()` | `opacity`, `rotation` (counter-clockwise; the box is the unrotated box) |
| `overflow` other than visible | `clip` |
| text-only element | `text` node: content, font family / weight / italic / size (+ style name), `lineHeight` px, `letterSpacing` px, colour, align, `text-transform` → case, underline / line-through → decoration, `text-overflow: ellipsis` → autoResize truncate, `-webkit-line-clamp` → maxLines |
| text-only element with background / border / padding | frame with a horizontal layout (padding, text-align → mainAlign) holding a `Label` text node |
| text beside elements | its own text node (`<parent id>#t<n>`) |
| `<svg>`, `<canvas>`, `<video>`, form controls | baked: the element's area cut from the preview (info lint) |
| CSS custom properties on `:root` used as colours | `tokens.color.<name>` + `colorToken` (only when the value matches) |

**Text autoResize.** In a flex parent: `width` when the item hugs, else `height` (wraps in a fixed / fill width).
In a canvas: `width` when the element shrink-wraps a single line, else `height`.
The text box is the line box (line-height x lines), so half-leading is the same on both sides.

**Fonts.** Text is drawn by the browser with whatever font the page loads and by UE with its font rules. For a fair
comparison use web fonts that UE also has; the conformance cases load UE's own Roboto:
`@font-face { font-family: "Roboto"; font-weight: 700; src: url("file:///C:/Program%20Files/Epic%20Games/UE_5.7/Engine/Content/Slate/Fonts/Roboto-Bold.ttf"); }`.
The engine Roboto has Light / Regular / Bold / Italic / Bold Italic faces (no Medium). A family that is not a loaded
web font gets an info lint. Each text writes `font.style` ("Bold", "Bold Italic", ...) besides `weight`.

## Identity and roles

- `id` = `data-db-id`, else the `id` attribute, else a hash of the DOM path (`h` + 10 hex). Capability
  `stable-ids` is set only when every element had an explicit id; otherwise an info lint says how many did not.
  Generated children derive their ids from their element (`<id>#text`, `<id>#t0`).
- `name` = `data-name`, else `id`, else `tag.firstClass`.
- `role` / `variable` / `widgetName` = `data-role` / `data-variable` / `data-widget-name`, then
  `dwocanvas.naming.apply_naming` fills the rest from the name, so `btn_`, `list_`, `$` ... prefixes work.
- The body: a plain block body is flattened into the document root (a viewport-sized frame with the page
  background); a plain flex / grid body that fills the viewport becomes the root's layout; otherwise it is a child.

## Not supported (lint)

Mixed inline text styles (flattened to the element's style), `::before` / `::after` content, more than one shadow of
a kind or stacked fills (UE keeps one), per-side border colours, transform scale / skew / 3D, `object-position`,
column `flex-wrap`, grid spans / explicit placement, `background-clip`, remote (http) images.

## Tests

```
python -m unittest discover -s canvas/adapters/html/tests -v
```

Mapping tests use synthetic extractor output. The browser test and the conformance file test are skipped when
Edge / Chrome or the cases are missing.

## Conformance cases

`plugin/DwoCanvas/Conformance/html_*/` hold `<case>.html` and the generated `<case>.dwocanvas.json` (preview
embedded, `x-conformance.maxDiff`). Rebuild them after changing the adapter or a page:

```
python canvas/adapters/html/build_conformance.py [case ...]
```

It regenerates the test textures (`html_images/photo.png`, `tile.png`) and keeps each file's `x-conformance` block.
The UE automation test `DwoCanvas.Conformance` imports every case, renders it and compares it with the preview.

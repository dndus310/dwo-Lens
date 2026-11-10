// DwoCanvas - Figma adapter: exports dwocanvas/2 JSON (plugin/DwoCanvas/Schema/dwocanvas.schema.json), lint and feature audit.
// Runs inside Figma (Plugins > Development > Import plugin from manifest...). No network access.
// Validate the result outside Figma with: python -m dwocanvas validate <file>.dwocanvas.json (canvas/python)

const SCHEMA = "dwocanvas/2";
const ADAPTER = "dwocanvas-figma/0.2.0";

// Team naming rules (layer-name prefix -> role / variable / widgetName).
// <naming.json> Must stay identical to canvas/python/dwocanvas/naming.json (checked by canvas/python/tests/test_figma_adapter.py)
const NAMING = {
  "version": 1,
  "description": "Default DwoCanvas team naming rules: layer-name prefix -> role. The longest matching prefix wins. The prefix is stripped from widgetName. variablePrefix marks a Blueprint variable.",
  "variablePrefix": "$",
  "prefixes": [
    {"prefix": "btn_", "role": "button", "variable": true},
    {"prefix": "list_", "role": "list", "variable": true},
    {"prefix": "tile_", "role": "tile", "variable": true},
    {"prefix": "scroll_", "role": "scroll", "variable": true},
    {"prefix": "input_", "role": "textInput", "variable": true},
    {"prefix": "chk_", "role": "checkBox", "variable": true},
    {"prefix": "slider_", "role": "slider", "variable": true},
    {"prefix": "progress_", "role": "progressBar", "variable": true},
    {"prefix": "switch_", "role": "switcher", "variable": true},
    {"prefix": "slot_", "role": "namedSlot", "variable": true},
    {"prefix": "spacer_", "role": "spacer", "variable": false},
    {"prefix": "_", "role": "ignore", "variable": false}
  ]
};
// </naming.json>

// Roles that are not Blueprint variables when set explicitly (plugin data role)
const NON_VARIABLE_ROLES = ["auto", "spacer", "ignore"];
// Plugin data "role" values: UE enum names (v1 convention) or v2 role names
const UE_ROLES = {
  Button: "button", ListView: "list", TileView: "tile", ScrollBox: "scroll", TextInput: "textInput", CheckBox: "checkBox",
  Slider: "slider", ProgressBar: "progressBar", Switcher: "switcher", NamedSlot: "namedSlot", Spacer: "spacer", Ignore: "ignore", Auto: "auto",
};
const V2_ROLES = ["auto", "button", "list", "tile", "scroll", "textInput", "checkBox", "slider", "progressBar", "switcher", "namedSlot", "spacer", "ignore"];
// What this adapter provides (dwocanvas/2 "capabilities"); "preview" is dropped when previews are off, "tokens" added when variables are bound
const EXTRACT_CAPABILITIES = ["layout.flow", "layout.constraints", "paint.gradient", "paint.image", "effects", "components", "variants", "preview", "stable-ids"];
const STATE_PROPERTY = /^state$/i;
const STATE_NAMES = { hover: "hovered", hovered: "hovered", pressed: "pressed", press: "pressed", disabled: "disabled" };

figma.showUI(__html__, { width: 380, height: 520 });

figma.ui.onmessage = async (msg) => {
  try {
    const roots = selectedRoots();
    if (roots.length === 0) {
      figma.ui.postMessage({ type: "error", message: "Select one or more top-level frames (screens) first." });
      return;
    }
    const ctx = newContext(msg.options || {});
    if (msg.type === "audit") {
      for (const root of roots) await audit(ctx, root);
      figma.ui.postMessage({ type: "audit", usage: ctx.usage });
      return;
    }

    for (const root of roots) {
      ctx.documents.push(await exportDocument(ctx, root, false));
    }
    // Components referenced by instances (and their own instances)
    for (let i = 0; i < ctx.componentQueue.length; i++) {
      const component = ctx.componentQueue[i];
      ctx.components.push(await exportDocument(ctx, component, true));
    }

    if (msg.type === "lint") {
      figma.ui.postMessage({ type: "lint", lint: ctx.lint });
      return;
    }

    const capabilities = EXTRACT_CAPABILITIES.filter((c) => c !== "preview" || ctx.options.previews);
    const hasTokens = Object.keys(ctx.tokens).length > 0;
    if (hasTokens) capabilities.push("tokens");
    const out = {
      schema: SCHEMA,
      source: { tool: "figma", adapter: ADAPTER, mode: "extract", file: figma.root.name, page: figma.currentPage.name, exportedAt: new Date().toISOString() },
      capabilities,
    };
    if (hasTokens) out.tokens = ctx.tokens;
    out.assets = ctx.assets;
    out.documents = ctx.documents;
    out.components = ctx.components;
    out.lint = ctx.lint;
    out.usage = ctx.usage;
    // The UI adds assets[*].sha256 (WebCrypto is not available in the plugin sandbox)
    figma.ui.postMessage({ type: "export", fileName: exportFileName(roots) + ".dwocanvas.json", json: JSON.stringify(out), lint: ctx.lint });
  } catch (error) {
    figma.ui.postMessage({ type: "error", message: String(error && error.stack ? error.stack : error) });
  }
};

function selectedRoots() {
  return figma.currentPage.selection.filter((n) => n.type === "FRAME" || n.type === "COMPONENT" || n.type === "COMPONENT_SET" || n.type === "INSTANCE");
}

function newContext(options) {
  return {
    options: {
      bakeScale: options.bakeScale || 2,
      previews: options.previews !== false,
      autoBake: options.autoBake !== false,
    },
    assets: {},
    assetIds: {},
    tokens: {},
    documents: [],
    components: [],
    componentQueue: [],
    componentIds: {},
    lint: [],
    usage: {},
    variableNames: {},
  };
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers

function sanitize(text) {
  return String(text).replace(/[^A-Za-z0-9_\-]+/g, "_");
}

/** File title, or the first selected frame when the title has no ASCII letters (e.g. "제목 없음" -> "_"). */
function exportFileName(roots) {
  for (const candidate of [figma.root.name, roots[0] && roots[0].name, "design"]) {
    const name = sanitize(candidate || "").replace(/^_+|_+$/g, "");
    if (/[A-Za-z0-9]/.test(name)) return name;
  }
  return "design";
}

function count(ctx, key) {
  ctx.usage[key] = (ctx.usage[key] || 0) + 1;
}

function lint(ctx, level, node, message, rule) {
  ctx.lint.push({ level, node: node.id, name: node.name, message, rule: rule || "figma" });
}

function color(c, opacity) {
  return [round(c.r), round(c.g), round(c.b), round((c.a === undefined ? 1 : c.a) * (opacity === undefined ? 1 : opacity))];
}

function round(v) {
  return Math.round(v * 10000) / 10000;
}

function hex2(v) {
  const n = Math.max(0, Math.min(255, Math.round(v * 255)));
  return (n < 16 ? "0" : "") + n.toString(16);
}

/** sRGB 0..1 color -> "#rrggbb" or "#rrggbbaa" (W3C design token color value). */
function hexColor(c) {
  const alpha = c.length > 3 && c[3] < 1 ? hex2(c[3]) : "";
  return "#" + hex2(c[0]) + hex2(c[1]) + hex2(c[2]) + alpha;
}

// Node metadata. Figma's setPluginData is private to the plugin that wrote it, so other tools (and the mockup builder)
// write shared plugin data: node.setSharedPluginData("dwocanvas", "class", "/Script/Module.Class").
// Private "db.<key>" plugin data written by this exporter itself is still read as a fallback.
const SHARED_NAMESPACE = "dwocanvas";
const META_KEYS = ["role", "variable", "bake", "class", "nineSlice", "loc"];

function metaValue(node, key) {
  let value = node.getSharedPluginData ? node.getSharedPluginData(SHARED_NAMESPACE, key) : "";
  if (!value && node.getPluginData) value = node.getPluginData("db." + key);
  return value;
}

function meta(node) {
  const m = {};
  for (const key of META_KEYS) {
    const value = metaValue(node, key);
    if (value) m[key] = value;
  }
  return m;
}

// ---------------------------------------------------------------------------------------------------------------------
// Naming rules -> role / variable / widgetName (same algorithm as canvas/python/dwocanvas/naming.py)

function sanitizeWidgetName(text) {
  const name = String(text === undefined || text === null ? "" : text).replace(/[^A-Za-z0-9_]/g, "_").replace(/_+/g, "_").replace(/^_+|_+$/g, "");
  if (!name) return "Widget";
  if (/^[0-9]/.test(name)) return "W_" + name;
  return name;
}

/** Longest matching prefix rule (case-sensitive), or null. */
function matchPrefix(name) {
  let best = null;
  for (const rule of NAMING.prefixes) {
    if (name.indexOf(rule.prefix) === 0 && (!best || rule.prefix.length > best.prefix.length)) best = rule;
  }
  return best;
}

/** { role, variable, widgetName, dollar } from a layer name: role prefix first (stripped), then the variable prefix. */
function resolveNaming(layerName) {
  let rest = String(layerName || "");
  let role = null;
  let variable = false;
  const rule = matchPrefix(rest);
  if (rule) {
    role = rule.role;
    variable = rule.variable === undefined ? NON_VARIABLE_ROLES.indexOf(role) < 0 : !!rule.variable;
    rest = rest.slice(rule.prefix.length);
  }
  const varPrefix = NAMING.variablePrefix === undefined ? "$" : NAMING.variablePrefix;
  const dollar = !!varPrefix && rest.indexOf(varPrefix) === 0;
  if (dollar) {
    rest = rest.slice(varPrefix.length);
    variable = true;
  }
  return { role, variable, widgetName: sanitizeWidgetName(rest), dollar };
}

/** Plugin data role (UE enum name, v2 role or x- role) -> v2 role, or null. */
function mapRole(value) {
  if (!value) return null;
  if (UE_ROLES[value]) return UE_ROLES[value];
  if (V2_ROLES.indexOf(value) >= 0 || /^x-[A-Za-z0-9_.-]+$/.test(value)) return value;
  return null;
}

function truthy(value) {
  return /^(true|yes|1|on)$/i.test(String(value || "").trim());
}

/** role / variable / widgetName of a node: plugin data "role" / "variable" win over the naming rules. */
function nodeNaming(node) {
  const m = meta(node);
  const byName = resolveNaming(node.name);
  const explicit = mapRole(m.role);
  const role = explicit || byName.role;
  let variable = explicit ? byName.dollar || NON_VARIABLE_ROLES.indexOf(explicit) < 0 : byName.variable;
  if (truthy(m.variable)) variable = true;
  return { role, variable, widgetName: byName.widgetName, unknownRole: m.role && !explicit ? m.role : null };
}

function roleOf(node) {
  return nodeNaming(node).role;
}

/** list / tile containers: their children are sample entries (one entry class), so repeated names are expected. */
function isRepeaterContainer(node) {
  const role = roleOf(node);
  return role === "list" || role === "tile";
}

// ---------------------------------------------------------------------------------------------------------------------
// Variables -> design tokens (colorToken path "color.primary" from the variable "color/primary")

function tokenPath(variableName) {
  return String(variableName).split("/").map((s) => s.trim().replace(/\./g, "_")).filter((s) => s.length > 0).join(".");
}

function addToken(ctx, path, value) {
  const parts = path.split(".");
  let group = ctx.tokens;
  for (let i = 0; i < parts.length - 1; i++) {
    if (group[parts[i]] === undefined) group[parts[i]] = {};
    group = group[parts[i]];
    if (group.$value !== undefined) return; // a token already uses this group name
  }
  const last = parts[parts.length - 1];
  if (group[last] === undefined) group[last] = { $type: "color", $value: hexColor(value) };
}

async function variableName(ctx, paint) {
  const binding = paint.boundVariables && paint.boundVariables.color;
  if (!binding || !binding.id) return undefined;
  if (ctx.variableNames[binding.id] !== undefined) return ctx.variableNames[binding.id];
  let name;
  try {
    const variable = await figma.variables.getVariableByIdAsync(binding.id);
    name = variable ? variable.name : undefined;
  } catch (e) {
    name = undefined;
  }
  ctx.variableNames[binding.id] = name;
  return name;
}

/** colorToken path of a bound color variable (registers the token definition with the resolved color), or undefined. */
async function colorToken(ctx, paint, resolvedColor) {
  const name = await variableName(ctx, paint);
  if (!name) return undefined;
  const path = tokenPath(name);
  if (!path) return undefined;
  addToken(ctx, path, resolvedColor);
  return path;
}

async function addImageBytes(ctx, key, bytes, scale) {
  if (ctx.assetIds[key]) return ctx.assetIds[key];
  const id = "img_" + Object.keys(ctx.assets).length;
  ctx.assets[id] = { data: figma.base64Encode(bytes), scale: scale || 1 };
  ctx.assetIds[key] = id;
  return id;
}

function invertAffine(m) {
  // m = [[a, c, e], [b, d, f]] maps node space (0..1) -> gradient space
  const a = m[0][0], c = m[0][1], e = m[0][2], b = m[1][0], d = m[1][1], f = m[1][2];
  const det = a * d - b * c || 1e-6;
  return [[d / det, -c / det, (c * f - d * e) / det], [-b / det, a / det, (b * e - a * f) / det]];
}

function apply(m, x, y) {
  return [m[0][0] * x + m[0][1] * y + m[0][2], m[1][0] * x + m[1][1] * y + m[1][2]];
}

// ---------------------------------------------------------------------------------------------------------------------
// Paints / strokes / effects

async function convertPaints(ctx, node, paints) {
  if (!paints || paints === figma.mixed) return [];
  const out = [];
  for (const paint of paints) {
    const visible = paint.visible !== false;
    if (paint.type === "SOLID") {
      count(ctx, "fill.solid");
      const p = { type: "solid", color: color(paint.color, 1), opacity: round(paint.opacity === undefined ? 1 : paint.opacity), visible };
      const token = await colorToken(ctx, paint, p.color);
      if (token) {
        p.colorToken = token;
        count(ctx, "fill.variable");
      }
      out.push(p);
    } else if (paint.type.indexOf("GRADIENT_") === 0) {
      const kind = paint.type.replace("GRADIENT_", "").toLowerCase();
      count(ctx, "fill.gradient." + kind);
      const inv = invertAffine(paint.gradientTransform);
      const start = apply(inv, 0, 0.5), end = apply(inv, 1, 0.5), width = apply(inv, 0, 1);
      out.push({
        type: kind,
        stops: paint.gradientStops.map((s) => ({ position: round(s.position), color: color(s.color, 1) })),
        handles: [start[0], start[1], end[0], end[1], width[0], width[1]].map(round),
        opacity: round(paint.opacity === undefined ? 1 : paint.opacity),
        visible,
      });
      if (paint.gradientStops.length > 4) lint(ctx, "warning", node, "Gradient has more than 4 stops (UE keeps 4): bake or simplify");
    } else if (paint.type === "IMAGE") {
      count(ctx, "fill.image." + (paint.scaleMode || "FILL").toLowerCase());
      let id;
      const image = paint.imageHash ? figma.getImageByHash(paint.imageHash) : null;
      if (image) {
        const bytes = await image.getBytesAsync();
        id = await addImageBytes(ctx, "hash:" + paint.imageHash, bytes, 1);
      }
      if (!id) lint(ctx, "warning", node, "Image fill has no image data (drawn empty)");
      out.push({ type: "image", image: id, scaleMode: (paint.scaleMode || "FILL").toLowerCase(), scalingFactor: paint.scalingFactor || 1, opacity: round(paint.opacity === undefined ? 1 : paint.opacity), visible });
    } else {
      count(ctx, "fill.unsupported." + paint.type);
      lint(ctx, "warning", node, "Paint type " + paint.type + " is not supported: bake the node");
    }
  }
  return out;
}

async function convertStroke(ctx, node) {
  if (!("strokes" in node) || !node.strokes || node.strokes.length === 0) return undefined;
  const paint = node.strokes.find((p) => p.visible !== false);
  if (!paint) return undefined;
  if (paint.type !== "SOLID") {
    lint(ctx, "warning", node, "Gradient / image stroke is not supported live: bake the node");
    return undefined;
  }
  count(ctx, "stroke." + String(node.strokeAlign || "INSIDE").toLowerCase());
  const stroke = {
    color: color(paint.color, paint.opacity),
    width: node.strokeWeight === figma.mixed ? node.strokeTopWeight : node.strokeWeight,
    align: String(node.strokeAlign || "INSIDE").toLowerCase(),
  };
  const token = await colorToken(ctx, paint, color(paint.color, 1));
  if (token) {
    stroke.colorToken = token;
    count(ctx, "stroke.variable");
  }
  if (node.strokeWeight === figma.mixed) {
    stroke.sides = [node.strokeLeftWeight, node.strokeTopWeight, node.strokeRightWeight, node.strokeBottomWeight];
    count(ctx, "stroke.perSide");
  }
  if (node.dashPattern && node.dashPattern.length >= 2) {
    stroke.dash = [node.dashPattern[0], node.dashPattern[1]];
    count(ctx, "stroke.dashed");
  }
  return stroke;
}

function convertRadius(node) {
  if (!("cornerRadius" in node)) return undefined;
  if (node.cornerRadius !== figma.mixed) {
    return node.cornerRadius ? [node.cornerRadius, node.cornerRadius, node.cornerRadius, node.cornerRadius] : undefined;
  }
  return [node.topLeftRadius, node.topRightRadius, node.bottomRightRadius, node.bottomLeftRadius];
}

function convertEffects(ctx, node) {
  if (!("effects" in node) || !node.effects) return [];
  const out = [];
  for (const effect of node.effects) {
    const type = effect.type.toLowerCase().replace("_", "-");
    count(ctx, "effect." + type);
    if (effect.type === "DROP_SHADOW" || effect.type === "INNER_SHADOW") {
      const shadow = { type, color: color(effect.color, 1), offset: [effect.offset.x, effect.offset.y], blur: effect.radius, spread: effect.spread || 0, visible: effect.visible !== false };
      // Drop shadow visible through translucent fills of the node itself
      if (effect.type === "DROP_SHADOW" && typeof effect.showShadowBehindNode === "boolean") shadow.showBehind = effect.showShadowBehindNode;
      out.push(shadow);
    } else if (effect.type === "BACKGROUND_BLUR" || effect.type === "LAYER_BLUR") {
      out.push({ type, radius: effect.radius, visible: effect.visible !== false });
    }
  }
  const shadows = node.effects.filter((e) => e.visible !== false && (e.type === "DROP_SHADOW")).length;
  if (shadows > 1) lint(ctx, "warning", node, shadows + " drop shadows: UE draws one live (bake for all)");
  return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Bake rules (B tier): exported as PNG with effects, positioned by its overflow

function bakeReason(ctx, node) {
  const m = meta(node);
  if (m.bake === "true" || /@bake$/.test(node.name)) return "marked for baking";
  if (m.bake === "false") return null;
  if (!ctx.options.autoBake) return null;
  if (["VECTOR", "BOOLEAN_OPERATION", "STAR", "POLYGON", "LINE"].indexOf(node.type) >= 0) return "vector shape";
  if (node.type === "ELLIPSE" && Math.abs(node.width - node.height) > 0.5) return "non-circular ellipse";
  if ("blendMode" in node && node.blendMode !== "NORMAL" && node.blendMode !== "PASS_THROUGH") return "blend mode " + node.blendMode;
  if ("effects" in node && node.effects.some((e) => e.visible !== false && e.type === "LAYER_BLUR")) return "layer blur";
  if ("children" in node && node.children.some((c) => c.isMask)) return "mask";
  if ("fills" in node && node.fills !== figma.mixed && node.fills.filter((p) => p.visible !== false).length > 1) return "stacked fills";
  if (node.type === "TEXT" && node.fills !== figma.mixed && node.fills.some((p) => p.visible !== false && p.type !== "SOLID")) return "gradient / image text";
  return null;
}

async function bake(ctx, node, reason) {
  count(ctx, "bake");
  const bytes = await node.exportAsync({ format: "PNG", constraint: { type: "SCALE", value: ctx.options.bakeScale } });
  const id = await addImageBytes(ctx, "bake:" + node.id, bytes, ctx.options.bakeScale);
  const box = node.absoluteBoundingBox;
  const render = node.absoluteRenderBounds || box;
  lint(ctx, "info", node, "Baked to image (" + reason + ")");
  return {
    image: id,
    overflow: [box.x - render.x, box.y - render.y, render.x + render.width - (box.x + box.width), render.y + render.height - (box.y + box.height)].map((v) => Math.max(0, round(v))),
  };
}

// ---------------------------------------------------------------------------------------------------------------------
// Nodes

const TYPE_MAP = {
  FRAME: "frame", GROUP: "group", COMPONENT: "component", COMPONENT_SET: "frame", INSTANCE: "instance",
  RECTANGLE: "rectangle", ELLIPSE: "ellipse", TEXT: "text", SECTION: "frame",
  VECTOR: "vector", BOOLEAN_OPERATION: "vector", STAR: "vector", POLYGON: "vector", LINE: "vector",
};

function lower(v, fallback) {
  return v ? String(v).toLowerCase() : fallback;
}

const ALIGN = { MIN: "start", CENTER: "center", MAX: "end", SPACE_BETWEEN: "space-between", BASELINE: "start", STRETCH: "stretch" };

async function convertNode(ctx, node, parentBox) {
  count(ctx, "node." + node.type.toLowerCase());
  const box = node.absoluteBoundingBox || { x: 0, y: 0, width: node.width, height: node.height };
  // Unrotated size around the rotated bounding box center
  const cx = box.x + box.width / 2 - parentBox.x;
  const cy = box.y + box.height / 2 - parentBox.y;
  const naming = nodeNaming(node);
  const out = {
    id: node.id,
    name: node.name,
    type: TYPE_MAP[node.type] || "frame",
  };
  if (naming.role) out.role = naming.role;
  out.variable = naming.variable;
  out.widgetName = naming.widgetName;
  out.visible = node.visible !== false;
  out.box = { x: round(cx - node.width / 2), y: round(cy - node.height / 2), w: round(Math.max(0, node.width)), h: round(Math.max(0, node.height)) };
  if (naming.unknownRole) lint(ctx, "warning", node, "Unknown role '" + naming.unknownRole + "' in plugin data: using the naming rules", "role");
  if (naming.role) count(ctx, "role." + naming.role);
  if ("opacity" in node && node.opacity < 1) out.opacity = round(node.opacity);
  if ("rotation" in node && Math.abs(node.rotation) > 0.01) {
    out.rotation = round(node.rotation);
    count(ctx, "rotation");
  }
  if ("clipsContent" in node && node.clipsContent) out.clip = true;
  if (!out.visible) count(ctx, "hidden");

  // role / variable are promoted to fields; other plugin data (bake, class, nineSlice, loc) stays in meta
  const m = meta(node);
  delete m.role;
  delete m.variable;
  if (Object.keys(m).length) out.meta = m;

  // Constraints / positioning
  if ("constraints" in node && node.constraints) {
    out.constraints = { h: lower(node.constraints.horizontal, "min"), v: lower(node.constraints.vertical, "min") };
  }
  if ("layoutPositioning" in node && node.layoutPositioning === "ABSOLUTE") {
    out.positioning = "absolute";
    count(ctx, "layout.absoluteChild");
  }
  if ("layoutSizingHorizontal" in node) {
    out.sizing = { h: lower(node.layoutSizingHorizontal, "fixed"), v: lower(node.layoutSizingVertical, "fixed") };
    if (node.minWidth) out.sizing.minW = node.minWidth;
    if (node.maxWidth) out.sizing.maxW = node.maxWidth;
    if (node.minHeight) out.sizing.minH = node.minHeight;
    if (node.maxHeight) out.sizing.maxH = node.maxHeight;
  }

  // Bake?
  const reason = bakeReason(ctx, node);
  if (reason) {
    out.bake = await bake(ctx, node, reason);
    return out;
  }

  // Auto layout
  if ("layoutMode" in node && node.layoutMode && node.layoutMode !== "NONE") {
    let mode = node.layoutMode === "GRID" ? "grid" : node.layoutMode.toLowerCase();
    if (node.layoutWrap === "WRAP") mode = "wrap";
    count(ctx, "layout." + mode);
    out.layout = {
      mode,
      gap: node.itemSpacing || 0,
      crossGap: node.counterAxisSpacing || 0,
      padding: [node.paddingLeft || 0, node.paddingTop || 0, node.paddingRight || 0, node.paddingBottom || 0],
      mainAlign: ALIGN[node.primaryAxisAlignItems] || "start",
      crossAlign: ALIGN[node.counterAxisAlignItems] || "start",
    };
    if (mode === "grid" && node.gridColumnCount) out.layout.columns = node.gridColumnCount;
    // Children are laid out inside the stroke too (padding measured from the stroke's inner edge)
    if (node.strokesIncludedInLayout) out.layout.strokesIncluded = true;
  } else if ("children" in node && node.type !== "GROUP" && node.type !== "INSTANCE" && node.children.length > 0) {
    count(ctx, "layout.none");
    if (node.children.length > 6) {
      lint(ctx, "warning", node, "Frame with " + node.children.length + " children has no auto layout: becomes absolute positions (use auto layout where content can grow)");
    }
  }

  // Visual
  if ("fills" in node && node.type !== "TEXT") {
    const fills = await convertPaints(ctx, node, node.fills);
    if (fills.length) out.fills = fills;
  }
  const stroke = await convertStroke(ctx, node);
  if (stroke) out.stroke = stroke;
  const radius = convertRadius(node);
  if (radius) {
    out.radius = radius;
    count(ctx, "radius");
  }
  if ("cornerSmoothing" in node && node.cornerSmoothing > 0) {
    count(ctx, "radius.smoothing");
    lint(ctx, "info", node, "Corner smoothing is drawn as a plain radius");
  }
  const effects = convertEffects(ctx, node);
  if (effects.length) out.effects = effects;

  // Text
  if (node.type === "TEXT") {
    out.text = await convertText(ctx, node);
    out.fills = await convertPaints(ctx, node, node.fills);
  }

  // Instance -> component reference (+ inline subtree for override handling)
  if (node.type === "INSTANCE") {
    const main = await node.getMainComponentAsync();
    if (main) {
      const set = main.parent && main.parent.type === "COMPONENT_SET" ? main.parent : null;
      out.component = {
        ref: main.id,
        name: set ? set.name : main.name,
        overridden: Array.isArray(node.overrides) && node.overrides.length > 0,
      };
      if (set) out.component.variant = node.variantProperties || {};
      if (!ctx.componentIds[main.id] && naming.role !== "button") {
        ctx.componentIds[main.id] = true;
        ctx.componentQueue.push(main);
      }
      count(ctx, out.component.overridden ? "instance.overridden" : "instance");
      if (naming.role === "button" && set) {
        out.states = await buttonStates(ctx, node, main, set);
      }
    }
  }

  if (naming.role === "button" && !out.states) {
    lint(ctx, "info", node, "Button without Hover / Pressed / Disabled variants: hover will look like normal");
  }

  // Children
  if ("children" in node && node.children.length) {
    out.children = [];
    const names = {};
    const repeater = isRepeaterContainer(node);
    for (const child of node.children) {
      if (child.isMask) continue;
      const childNaming = nodeNaming(child);
      if (childNaming.role !== "ignore") {
        const widgetName = childNaming.widgetName;
        if (names[widgetName] && !repeater) lint(ctx, "warning", child, "Duplicate widget name '" + widgetName + "' under '" + node.name + "': widget names get a suffix", "widget-name-duplicate");
        names[widgetName] = true;
      }
      out.children.push(await convertNode(ctx, child, box));
    }
  }

  if (/[^A-Za-z0-9_$]/.test(node.name) && !/^_/.test(node.name)) {
    count(ctx, "naming.nonAscii");
  }
  return out;
}

/** Hover / Pressed / Disabled siblings of a variant (same other properties). */
async function buttonStates(ctx, instance, main, set) {
  const props = main.variantProperties || {};
  const stateKey = Object.keys(props).find((k) => STATE_PROPERTY.test(k));
  if (!stateKey) return undefined;
  const states = {};
  for (const variant of set.children) {
    const vp = variant.variantProperties || {};
    const same = Object.keys(props).every((k) => k === stateKey || vp[k] === props[k]);
    const state = STATE_NAMES[String(vp[stateKey] || "").toLowerCase()];
    if (!same || !state || variant.id === main.id) continue;
    const visual = { fills: await convertPaints(ctx, variant, variant.fills), radius: convertRadius(variant) };
    const stroke = await convertStroke(ctx, variant);
    if (stroke) visual.stroke = stroke;
    const effects = convertEffects(ctx, variant);
    if (effects.some((e) => e.type === "drop-shadow" || e.type === "inner-shadow") || visual.fills.some((f) => f.type !== "solid")) {
      visual.bake = await bake(ctx, variant, "button state with gradient / shadow");
    }
    states[state] = visual;
  }
  count(ctx, "button.states");
  return states;
}

// Font style name -> CSS weight / italic (same table as canvas/python/dwocanvas/migrate.py font_weight)
const FONT_WEIGHTS = [
  [["extralight", "ultralight"], 200],
  [["semibold", "demibold"], 600],
  [["extrabold", "ultrabold", "heavy"], 800],
  [["thin", "hairline"], 100],
  [["light"], 300],
  [["medium"], 500],
  [["bold"], 700],
  [["black"], 900],
  [["regular", "book", "normal"], 400],
];

function fontStyle(styleName) {
  const text = String(styleName || "").replace(/[\s_\-]+/g, "").toLowerCase();
  const italic = text.indexOf("italic") >= 0 || text.indexOf("oblique") >= 0;
  for (const entry of FONT_WEIGHTS) {
    if (entry[0].some((w) => text.indexOf(w) >= 0)) return { weight: entry[1], italic };
  }
  return { weight: 400, italic };
}

async function convertText(ctx, node) {
  const font = node.fontName !== figma.mixed ? node.fontName : node.getRangeFontName(0, 1);
  const size = node.fontSize !== figma.mixed ? node.fontSize : node.getRangeFontSize(0, 1);
  const style = fontStyle(font.style);
  const weight = typeof node.fontWeight === "number" ? node.fontWeight : style.weight;
  const text = {
    content: node.characters,
    font: { family: font.family, weight, italic: style.italic, size, style: font.style },
    align: lower(node.textAlignHorizontal, "left").replace("justified", "justify"),
    valign: lower(node.textAlignVertical, "top"),
    autoResize: { NONE: "none", WIDTH_AND_HEIGHT: "width", HEIGHT: "height", TRUNCATE: "truncate" }[node.textAutoResize] || "none",
  };
  ctx.usage["font." + font.family + " " + font.style] = (ctx.usage["font." + font.family + " " + font.style] || 0) + 1;

  if (node.textTruncation === "ENDING") text.autoResize = text.autoResize === "none" ? "truncate" : text.autoResize;
  if (node.maxLines) text.maxLines = node.maxLines;

  const lh = node.lineHeight !== figma.mixed ? node.lineHeight : null;
  if (lh && lh.unit !== "AUTO") text.lineHeight = { unit: lh.unit === "PIXELS" ? "px" : "percent", value: lh.value };
  const ls = node.letterSpacing !== figma.mixed ? node.letterSpacing : null;
  if (ls && ls.value) text.letterSpacing = { unit: ls.unit === "PIXELS" ? "px" : "percent", value: ls.value };

  if (node.textCase && node.textCase !== figma.mixed && node.textCase !== "ORIGINAL") {
    text.case = { UPPER: "upper", LOWER: "lower", TITLE: "title" }[node.textCase] || "none";
    count(ctx, "text.case." + text.case);
  }
  if (node.textDecoration && node.textDecoration !== figma.mixed && node.textDecoration !== "NONE") {
    text.decoration = node.textDecoration === "UNDERLINE" ? "underline" : "strike";
    count(ctx, "text.decoration");
    lint(ctx, "warning", node, "Text decoration needs a RichTextBlock style in UE (not applied automatically)");
  }

  const segments = node.getStyledTextSegments(["fontName", "fontSize", "fills"]);
  if (segments.length > 1) {
    text.runs = segments.map((s) => {
      const runStyle = fontStyle(s.fontName.style);
      return { start: s.start, end: s.end, font: { family: s.fontName.family, weight: runStyle.weight, italic: runStyle.italic, size: s.fontSize, style: s.fontName.style } };
    });
    count(ctx, "text.mixedStyles");
    lint(ctx, "warning", node, "Mixed text styles: UE imports the first style (use a RichTextBlock for mixed styles)");
  }

  const loc = metaValue(node, "loc");
  if (loc && loc.indexOf(":") > 0) {
    text.loc = { table: loc.split(":")[0], key: loc.split(":").slice(1).join(":") };
  }
  count(ctx, "text");
  return text;
}

async function exportDocument(ctx, root, isComponent) {
  const box = root.absoluteBoundingBox;
  const rootNode = await convertNode(ctx, root, { x: box.x, y: box.y });
  rootNode.box.x = 0;
  rootNode.box.y = 0;
  const doc = { id: root.id, name: root.parent && root.parent.type === "COMPONENT_SET" ? root.parent.name + "_" + sanitize(Object.values(root.variantProperties || {}).join("_")) : root.name, kind: isComponent ? "component" : "screen", root: rootNode };
  const m = meta(root);
  if (m.class) doc.parentClass = m.class;
  if (ctx.options.previews && !isComponent) {
    const bytes = await root.exportAsync({ format: "PNG", constraint: { type: "SCALE", value: 1 } });
    doc.preview = await addImageBytes(ctx, "preview:" + root.id, bytes, 1);
  }
  return doc;
}

// ---------------------------------------------------------------------------------------------------------------------
// Audit: feature usage counts (decide what to support first)

async function audit(ctx, node) {
  const box = node.absoluteBoundingBox || { x: 0, y: 0 };
  const previousAutoBake = ctx.options.autoBake;
  ctx.options.autoBake = false; // count features, export nothing
  ctx.options.previews = false;
  await walk(ctx, node, box);
  ctx.options.autoBake = previousAutoBake;
}

async function walk(ctx, node, parentBox) {
  count(ctx, "node." + node.type.toLowerCase());
  if ("layoutMode" in node && node.layoutMode && node.layoutMode !== "NONE") count(ctx, "layout." + (node.layoutWrap === "WRAP" ? "wrap" : node.layoutMode.toLowerCase()));
  if ("fills" in node && node.fills !== figma.mixed) {
    for (const p of node.fills) count(ctx, "fill." + p.type.toLowerCase());
    if (node.fills.filter((p) => p.visible !== false).length > 1) count(ctx, "fill.stacked");
  }
  if ("effects" in node) for (const e of node.effects) count(ctx, "effect." + e.type.toLowerCase());
  if ("blendMode" in node && node.blendMode !== "NORMAL" && node.blendMode !== "PASS_THROUGH") count(ctx, "blend." + node.blendMode.toLowerCase());
  if ("dashPattern" in node && node.dashPattern && node.dashPattern.length) count(ctx, "stroke.dashed");
  if (node.type === "TEXT") {
    count(ctx, "text");
    if (node.getStyledTextSegments(["fontName"]).length > 1) count(ctx, "text.mixedStyles");
  }
  if (node.isMask) count(ctx, "mask");
  if ("children" in node) for (const child of node.children) await walk(ctx, child, node.absoluteBoundingBox || parentBox);
}

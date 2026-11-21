// Builds the DWO_Proj_01 Attribute Sample UI as a Figma design (the "시안"), following the DwoCanvas naming rules (canvas/python/dwocanvas/naming.json).
// Run once on an empty page, then select the frames and run "DwoCanvas Export".

const C = {
  panel: { r: 0.11, g: 0.12, b: 0.14 },
  section: { r: 0.2, g: 0.22, b: 0.25 },
  button: { r: 0.46, g: 0.48, b: 0.53 },
  buttonHover: { r: 0.55, g: 0.57, b: 0.62 },
  buttonPressed: { r: 0.38, g: 0.4, b: 0.44 },
  primary: { r: 0.35, g: 0.7, b: 0.98 },
  primaryHover: { r: 0.45, g: 0.77, b: 1 },
  primaryPressed: { r: 0.25, g: 0.6, b: 0.9 },
  disabled: { r: 0.3, g: 0.31, b: 0.33 },
  building: { r: 0.35, g: 0.77, b: 1 },
  load: { r: 1, g: 0.7, b: 0.25 },
  text: { r: 0.96, g: 0.96, b: 0.97 },
  sub: { r: 0.77, g: 0.79, b: 0.82 },
  scene: { r: 0.42, g: 0.55, b: 0.72 },
};

function solid(c, opacity) {
  return [{ type: "SOLID", color: c, opacity: opacity === undefined ? 1 : opacity }];
}

function frame(name, opts) {
  const f = figma.createFrame();
  f.name = name;
  f.fills = opts.fill ? solid(opts.fill, opts.fillOpacity) : [];
  if (opts.radius) f.cornerRadius = opts.radius;
  if (opts.layout) {
    f.layoutMode = opts.layout;
    f.itemSpacing = opts.gap || 0;
    const p = opts.padding || [0, 0, 0, 0];
    f.paddingLeft = p[0]; f.paddingTop = p[1]; f.paddingRight = p[2]; f.paddingBottom = p[3];
    f.primaryAxisAlignItems = opts.main || "MIN";
    f.counterAxisAlignItems = opts.cross || "MIN";
    f.primaryAxisSizingMode = opts.hugMain ? "AUTO" : "FIXED";
    f.counterAxisSizingMode = opts.hugCross ? "AUTO" : "FIXED";
    if (opts.wrap) { f.layoutWrap = "WRAP"; f.counterAxisSpacing = opts.crossGap || 0; }
  }
  if (opts.w && opts.h) f.resize(opts.w, opts.h);
  return f;
}

function text(name, content, size, style, color, opts) {
  const t = figma.createText();
  t.name = name;
  t.fontName = { family: "Roboto", style: style || "Regular" };
  t.characters = content;
  t.fontSize = size;
  t.fills = solid(color || C.text);
  if (opts && opts.upper) t.textCase = "UPPER";
  if (opts && opts.align) t.textAlignHorizontal = opts.align;
  if (opts && opts.width) { t.textAutoResize = "HEIGHT"; t.resize(opts.width, t.height); }
  if (opts && opts.truncate) { t.textAutoResize = "TRUNCATE"; t.textTruncation = "ENDING"; t.resize(opts.truncate, t.height); }
  return t;
}

function fillChild(node, h, v) {
  if (h) node.layoutSizingHorizontal = h;
  if (v) node.layoutSizingVertical = v;
}

// ---------------------------------------------------------------------------------------------------------------------
// Components

function buttonVariant(name, state, fill, label) {
  const c = figma.createComponent();
  c.name = "State=" + state;
  c.layoutMode = "HORIZONTAL";
  c.primaryAxisAlignItems = "CENTER";
  c.counterAxisAlignItems = "CENTER";
  c.paddingLeft = c.paddingRight = 12;
  c.resize(120, 32);
  c.primaryAxisSizingMode = "FIXED";
  c.counterAxisSizingMode = "FIXED";
  c.cornerRadius = 6;
  c.fills = solid(fill);
  c.appendChild(text("Label", label, 13, "Bold", state === "Disabled" ? C.sub : C.text));
  return c;
}

function buttonSet(name, colors, label) {
  const variants = [
    buttonVariant(name, "Default", colors[0], label),
    buttonVariant(name, "Hover", colors[1], label),
    buttonVariant(name, "Pressed", colors[2], label),
    buttonVariant(name, "Disabled", C.disabled, label),
  ];
  const set = figma.combineAsVariants(variants, figma.currentPage);
  set.name = name;
  set.layoutMode = "HORIZONTAL";
  set.itemSpacing = 16;
  set.paddingLeft = set.paddingTop = set.paddingRight = set.paddingBottom = 16;
  set.primaryAxisSizingMode = "AUTO";
  set.counterAxisSizingMode = "AUTO";
  return set;
}

function listEntry() {
  const c = figma.createComponent();
  c.name = "AttributeListEntry";
  c.layoutMode = "HORIZONTAL";
  c.counterAxisAlignItems = "CENTER";
  c.itemSpacing = 8;
  c.paddingLeft = 8; c.paddingRight = 10;
  c.resize(264, 30);
  c.primaryAxisSizingMode = "FIXED";
  c.counterAxisSizingMode = "FIXED";
  c.cornerRadius = 4;
  c.fills = solid(C.button);
  const bar = figma.createRectangle();
  bar.name = "LayerBar";
  bar.resize(4, 18);
  bar.fills = solid(C.building);
  c.appendChild(bar);
  const name = text("$EntryName", "Box Building", 12, "Medium", C.text);
  c.appendChild(name);
  fillChild(name, "FILL");
  c.appendChild(text("$EntryTag", "DT", 9, "Regular", C.sub));
  return c;
}

function assetItem() {
  const c = figma.createComponent();
  c.name = "AttributeAssetItem";
  c.layoutMode = "VERTICAL";
  c.counterAxisAlignItems = "CENTER";
  c.itemSpacing = 4;
  c.paddingLeft = c.paddingRight = c.paddingTop = c.paddingBottom = 6;
  c.resize(120, 146);
  c.primaryAxisSizingMode = "FIXED";
  c.counterAxisSizingMode = "FIXED";
  c.cornerRadius = 6;
  c.fills = solid(C.section);
  c.strokes = solid(C.text, 0.08);
  c.strokeWeight = 1;
  c.strokeAlign = "INSIDE";

  const thumb = figma.createFrame();
  thumb.name = "$Thumbnail";
  thumb.resize(96, 96);
  thumb.cornerRadius = 4;
  thumb.fills = [{
    type: "GRADIENT_LINEAR",
    gradientTransform: [[0, 1, 0], [-1, 0, 1]],
    gradientStops: [{ position: 0, color: { r: 0.32, g: 0.36, b: 0.44, a: 1 } }, { position: 1, color: { r: 0.12, g: 0.13, b: 0.16, a: 1 } }],
  }];
  const shape = figma.createEllipse();
  shape.name = "Preview";
  shape.resize(48, 48);
  shape.x = 24; shape.y = 24;
  shape.fills = solid(C.sub);
  thumb.appendChild(shape);
  c.appendChild(thumb);

  const name = text("$ItemName", "Box Building", 11, "Bold", C.text, { align: "CENTER", truncate: 108 });
  c.appendChild(name);
  c.appendChild(text("$ItemTag", "BUILDING", 9, "Regular", C.building, { align: "CENTER" }));
  return c;
}

// ---------------------------------------------------------------------------------------------------------------------
// Screens

function tabs(tabSet) {
  const row = frame("Layer", { layout: "HORIZONTAL", gap: 4, hugCross: true });
  const building = tabSet.children.find((v) => v.name === "State=Default").createInstance();
  building.name = "btn_BuildingTab";
  building.findOne((n) => n.type === "TEXT").characters = "Building";
  building.fills = solid(C.building);
  const load = tabSet.children.find((v) => v.name === "State=Default").createInstance();
  load.name = "btn_LoadTab";
  load.findOne((n) => n.type === "TEXT").characters = "Load";
  row.appendChild(building);
  row.appendChild(load);
  fillChild(building, "FILL");
  fillChild(load, "FILL");
  return row;
}

function leftPanel(tabSet, primarySet, entry) {
  const p = frame("LeftPanel", { layout: "VERTICAL", gap: 0, padding: [14, 14, 14, 14], fill: C.panel, fillOpacity: 0.9, w: 300, h: 868 });
  p.cornerRadius = 8;
  p.effects = [{ type: "DROP_SHADOW", color: { r: 0, g: 0, b: 0, a: 0.35 }, offset: { x: 0, y: 6 }, radius: 18, spread: 0, visible: true, blendMode: "NORMAL" }];

  p.appendChild(text("Title", "Attributes", 16, "Bold", C.text, { upper: true }));
  const hint = text("Hint", "Click an item to spawn it", 9, "Regular", C.sub);
  p.appendChild(hint);

  const spacer1 = frame("spacer_gap1", { w: 10, h: 10 });
  p.appendChild(spacer1);
  const layer = tabs(tabSet);
  p.appendChild(layer);
  fillChild(layer, "FILL");
  const spacer2 = frame("spacer_gap2", { w: 10, h: 10 });
  p.appendChild(spacer2);

  const listBg = frame("list_AttributeList", { layout: "VERTICAL", gap: 4, padding: [6, 6, 6, 6], fill: C.section });
  listBg.cornerRadius = 4;
  p.appendChild(listBg);
  fillChild(listBg, "FILL", "FILL");
  const names = [["Box Building", C.building], ["Chamfer Block", C.building], ["Silo", C.building], ["Quarter Hall", C.building], ["Cone Tower", C.building]];
  for (const [label, color] of names) {
    const row = entry.createInstance();
    row.findOne((n) => n.name === "$EntryName").characters = label;
    row.findOne((n) => n.name === "LayerBar").fills = solid(color);
    listBg.appendChild(row);
    fillChild(row, "FILL");
  }

  const add = primarySet.children.find((v) => v.name === "State=Default").createInstance();
  add.name = "btn_AddAttribute";
  add.findOne((n) => n.type === "TEXT").characters = "+  Add Attribute";
  p.appendChild(frame("spacer_gap3", { w: 10, h: 10 }));
  p.appendChild(add);
  fillChild(add, "FILL");
  add.resize(add.width, 36);

  const status = text("$StatusText", "Spawned Box Building (BUILDING)", 9, "Regular", C.sub, { width: 272 });
  p.appendChild(frame("spacer_gap4", { w: 10, h: 8 }));
  p.appendChild(status);
  fillChild(status, "FILL");
  return p;
}

function assetList(tabSet, primarySet, buttonSetNode, item) {
  const p = frame("AttributeAssetList", { layout: "VERTICAL", gap: 8, hugMain: true, w: 412, h: 700 });
  const tiles = frame("tile_List", { layout: "HORIZONTAL", wrap: true, gap: 8, crossGap: 8, padding: [4, 4, 4, 4], fill: C.section, w: 412, h: 600 });
  tiles.cornerRadius = 4;
  tiles.clipsContent = true;
  const items = [["Box Building", "BUILDING", C.building], ["Chamfer Block", "BUILDING", C.building], ["Silo", "BUILDING", C.building],
    ["Quarter Hall", "BUILDING", C.building], ["Cone Tower", "BUILDING", C.building], ["car", "BUILDING | USER", C.building]];
  for (const [label, tag, color] of items) {
    const i = item.createInstance();
    i.findOne((n) => n.name === "$ItemName").characters = label;
    const tagNode = i.findOne((n) => n.name === "$ItemTag");
    tagNode.characters = tag;
    tagNode.fills = solid(color);
    tiles.appendChild(i);
  }
  p.appendChild(tiles);
  fillChild(tiles, "FILL", "FIXED");

  p.appendChild(text("$StatusText", "Select an asset, then Apply.", 9, "Regular", C.sub));

  const buttons = frame("Buttons", { layout: "HORIZONTAL", gap: 6, hugCross: true });
  const imp = buttonSetNode.children.find((v) => v.name === "State=Default").createInstance();
  imp.name = "btn_ImportUserAsset";
  imp.findOne((n) => n.type === "TEXT").characters = "Import User Asset...";
  const apply = primarySet.children.find((v) => v.name === "State=Default").createInstance();
  apply.name = "btn_Apply";
  apply.findOne((n) => n.type === "TEXT").characters = "Apply";
  buttons.appendChild(imp);
  buttons.appendChild(apply);
  fillChild(imp, "FILL");
  fillChild(apply, "FILL");
  p.appendChild(buttons);
  fillChild(buttons, "FILL");
  // Bind to the project's C++ widget (BindWidgetOptional: List, Apply, ImportUserAsset, StatusText)
  // Shared plugin data: private setPluginData would only be visible to this builder, not to the exporter
  p.setSharedPluginData("dwocanvas", "class", "/Script/DWO_Proj_01.AttributeAssetListWidget");
  return p;
}

function rightPanel(tabSet, primarySet, buttonSetNode, item) {
  const p = frame("RightPanel", { layout: "VERTICAL", gap: 0, padding: [14, 14, 14, 14], fill: C.panel, fillOpacity: 0.9, w: 440, h: 868 });
  p.cornerRadius = 8;
  p.effects = [{ type: "DROP_SHADOW", color: { r: 0, g: 0, b: 0, a: 0.35 }, offset: { x: 0, y: 6 }, radius: 18, spread: 0, visible: true, blendMode: "NORMAL" }];

  const header = frame("Header", { layout: "HORIZONTAL", cross: "CENTER", hugCross: true });
  const title = text("Title", "Asset Library", 16, "Bold", C.text, { upper: true });
  header.appendChild(title);
  fillChild(title, "FILL");
  const close = buttonSetNode.children.find((v) => v.name === "State=Default").createInstance();
  close.name = "btn_CloseButton";
  close.findOne((n) => n.type === "TEXT").characters = "X";
  close.resize(32, 32);
  header.appendChild(close);
  p.appendChild(header);
  fillChild(header, "FILL");

  p.appendChild(text("Hint", "Pick an asset and Apply to add it to the attribute list", 9, "Regular", C.sub));
  p.appendChild(frame("spacer_gap1", { w: 10, h: 10 }));
  const layer = tabs(tabSet);
  p.appendChild(layer);
  fillChild(layer, "FILL");
  p.appendChild(frame("spacer_gap2", { w: 10, h: 10 }));

  const list = assetList(tabSet, primarySet, buttonSetNode, item);
  p.appendChild(list);
  fillChild(list, "FILL");
  return p;
}

async function main() {
  for (const style of ["Regular", "Medium", "Bold"]) {
    await figma.loadFontAsync({ family: "Roboto", style });
  }
  const page = figma.currentPage;

  // Components area
  const tabSet = buttonSet("TabButton", [C.button, C.buttonHover, C.buttonPressed], "Tab");
  const primarySet = buttonSet("PrimaryButton", [C.primary, C.primaryHover, C.primaryPressed], "Apply");
  const plainSet = buttonSet("Button", [C.button, C.buttonHover, C.buttonPressed], "Button");
  const entry = listEntry();
  const item = assetItem();
  tabSet.x = 0; tabSet.y = 1000;
  primarySet.x = 0; primarySet.y = 1100;
  plainSet.x = 0; plainSet.y = 1200;
  entry.x = 700; entry.y = 1000;
  item.x = 1000; item.y = 1000;

  // Screen: 1600 x 900 viewport, panels docked left / right (scene shows through)
  const screen = frame("AttributeScreen", { fill: C.scene, w: 1600, h: 900 });
  screen.clipsContent = true;
  const left = leftPanel(tabSet, primarySet, entry);
  screen.appendChild(left);
  left.x = 16; left.y = 16;
  left.constraints = { horizontal: "MIN", vertical: "STRETCH" };
  const right = rightPanel(tabSet, primarySet, plainSet, item);
  screen.appendChild(right);
  right.x = 1600 - 16 - 440; right.y = 16;
  right.constraints = { horizontal: "MAX", vertical: "STRETCH" };

  // Floor grid decoration (vector -> baked)
  const marker = figma.createStar();
  marker.name = "SpawnMarker";
  marker.resize(40, 40);
  marker.fills = solid(C.load);
  marker.effects = [{ type: "DROP_SHADOW", color: { r: 1, g: 0.6, b: 0.1, a: 0.6 }, offset: { x: 0, y: 0 }, radius: 12, spread: 0, visible: true, blendMode: "NORMAL" }];
  screen.appendChild(marker);
  marker.x = 780; marker.y = 430;
  marker.constraints = { horizontal: "CENTER", vertical: "CENTER" };

  page.appendChild(screen);
  screen.x = 0; screen.y = 0;

  // Standalone asset list bound to the C++ widget
  const standalone = assetList(tabSet, primarySet, plainSet, item);
  standalone.x = 1700; standalone.y = 0;
  page.appendChild(standalone);

  page.selection = [screen, standalone];
  figma.viewport.scrollAndZoomIntoView([screen, standalone]);
  figma.closePlugin("DWO mockup built: AttributeScreen + AttributeAssetList (selected)");
}

main().catch((e) => figma.closePlugin("Failed: " + (e && e.message ? e.message : e)));

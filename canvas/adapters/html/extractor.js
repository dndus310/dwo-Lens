// DwoCanvas HTML adapter - layout extractor, injected into a copy of the page.
// Runs after fonts and images are ready, walks the DOM with getComputedStyle / getBoundingClientRect and
// hands a raw JSON tree to the host page (postMessage to the wrapper frame, or a <pre id="dwocanvas-output">).
// The Python side (html2dwocanvas.py) maps the raw tree to dwocanvas/2. Keep this file free of mapping logic.
(function () {
  'use strict';

  var ATTRS = ['id', 'class', 'data-db-id', 'data-name', 'data-role', 'data-variable', 'data-widget-name'];
  var STYLE = [
    'display', 'position', 'visibility', 'opacity', 'zIndex', 'overflowX', 'overflowY', 'boxSizing',
    'width', 'height', 'minWidth', 'maxWidth', 'minHeight', 'maxHeight',
    'flexDirection', 'flexWrap', 'justifyContent', 'alignItems', 'alignSelf', 'alignContent', 'flexGrow', 'flexShrink', 'flexBasis',
    'rowGap', 'columnGap', 'gridTemplateColumns', 'gridTemplateRows', 'gridAutoFlow',
    'paddingLeft', 'paddingTop', 'paddingRight', 'paddingBottom', 'marginLeft', 'marginTop', 'marginRight', 'marginBottom',
    'borderLeftWidth', 'borderTopWidth', 'borderRightWidth', 'borderBottomWidth',
    'borderLeftColor', 'borderTopColor', 'borderRightColor', 'borderBottomColor',
    'borderLeftStyle', 'borderTopStyle', 'borderRightStyle', 'borderBottomStyle',
    'borderTopLeftRadius', 'borderTopRightRadius', 'borderBottomRightRadius', 'borderBottomLeftRadius',
    'backgroundColor', 'backgroundImage', 'backgroundSize', 'backgroundRepeat', 'backgroundPosition', 'backgroundClip',
    'boxShadow', 'backdropFilter', 'filter', 'mixBlendMode', 'transform', 'transformOrigin',
    'color', 'fontFamily', 'fontWeight', 'fontStyle', 'fontSize', 'lineHeight', 'letterSpacing', 'textAlign',
    'textTransform', 'textOverflow', 'whiteSpace', 'webkitLineClamp', 'textDecorationLine', 'textShadow',
    'objectFit', 'objectPosition'
  ];
  var SKIP = { SCRIPT: 1, STYLE: 1, LINK: 1, META: 1, NOSCRIPT: 1, TEMPLATE: 1, HEAD: 1, TITLE: 1, BR: 1, WBR: 1 };
  var INLINE = { SPAN: 1, B: 1, I: 1, EM: 1, STRONG: 1, A: 1, SMALL: 1, U: 1, S: 1, SUB: 1, SUP: 1, MARK: 1, CODE: 1, LABEL: 1, ABBR: 1, Q: 1, CITE: 1 };
  var REPLACED = { IMG: 1, SVG: 1, CANVAS: 1, VIDEO: 1, IFRAME: 1, OBJECT: 1, EMBED: 1, INPUT: 1, TEXTAREA: 1, SELECT: 1, BUTTON: 0 };
  var COLOR_PROPS = ['background-color', 'background', 'color', 'border-color', 'border-top-color', 'border'];

  var warnings = [];

  function round(v) { return Math.round(v * 100) / 100; }
  function rectOf(el) {
    var r = el.getBoundingClientRect();
    return { x: round(r.left), y: round(r.top), w: round(r.width), h: round(r.height) };
  }
  function pickStyle(cs) {
    var out = {};
    for (var i = 0; i < STYLE.length; i++) { out[STYLE[i]] = cs[STYLE[i]]; }
    return out;
  }
  function attrsOf(el) {
    var out = {};
    for (var i = 0; i < ATTRS.length; i++) {
      var v = el.getAttribute(ATTRS[i]);
      if (v !== null) { out[ATTRS[i]] = v; }
    }
    return out;
  }
  function domPath(el) {
    var parts = [];
    while (el && el.nodeType === 1 && el !== document.documentElement) {
      var index = 1, sib = el;
      while ((sib = sib.previousElementSibling)) { index++; }
      parts.unshift(el.tagName.toLowerCase() + ':' + index);
      el = el.parentElement;
    }
    return parts.join('>');
  }
  function transparent(color) { return !color || color === 'transparent' || /rgba\([^)]*,\s*0\)$/.test(color) || / \/ 0\)$/.test(color); }
  function hasVisual(cs) {
    if (!transparent(cs.backgroundColor) || cs.backgroundImage !== 'none' || cs.boxShadow !== 'none') { return true; }
    var sides = ['Left', 'Top', 'Right', 'Bottom'];
    for (var i = 0; i < 4; i++) {
      if (parseFloat(cs['border' + sides[i] + 'Width']) > 0 && cs['border' + sides[i] + 'Style'] !== 'none') { return true; }
    }
    return false;
  }

  // ---------------------------------------------------------------------------------------------------------
  // Transforms: measure everything untransformed; remember each element's transformed centre and matrix

  var transforms = new Map();
  function freezeTransforms() {
    var all = document.body ? document.body.querySelectorAll('*') : [];
    var list = [document.body].concat(Array.prototype.slice.call(all));
    for (var i = 0; i < list.length; i++) {   // document order: ancestors first
      var el = list[i];
      if (!el || !el.style) { continue; }
      var cs = getComputedStyle(el);
      if (!cs.transform || cs.transform === 'none') { continue; }
      var r = el.getBoundingClientRect();
      transforms.set(el, { matrix: cs.transform, origin: cs.transformOrigin, cx: r.left + r.width / 2, cy: r.top + r.height / 2 });
      el.style.setProperty('transform', 'none', 'important');
    }
  }

  // ---------------------------------------------------------------------------------------------------------
  // Text

  function isPlainInline(el) {
    if (!INLINE[el.tagName]) { return false; }
    var cs = getComputedStyle(el);
    if (cs.display !== 'inline' || hasVisual(cs)) { return false; }
    for (var c = el.firstElementChild; c; c = c.nextElementSibling) {
      if (c.tagName !== 'BR' && !isPlainInline(c)) { return false; }
    }
    return true;
  }
  /** Element whose content is text only (plus <br> and unstyled inline elements). */
  function isTextLeaf(el) {
    if (REPLACED[el.tagName]) { return false; }
    var text = false;
    for (var n = el.firstChild; n; n = n.nextSibling) {
      if (n.nodeType === 3) { if (/\S/.test(n.data)) { text = true; } }
      else if (n.nodeType === 1) {
        if (n.tagName === 'BR' || n.tagName === 'WBR' || SKIP[n.tagName]) { continue; }
        if (!isPlainInline(n)) { return false; }
        if (/\S/.test(n.textContent)) { text = true; }
      }
    }
    return text;
  }
  function rawText(node) {
    if (node.nodeType === 3) { return node.data; }
    if (node.nodeType !== 1 || SKIP[node.tagName] && node.tagName !== 'BR') { return ''; }
    if (node.tagName === 'BR') { return '\u2028'; }
    var s = '';
    for (var n = node.firstChild; n; n = n.nextSibling) { s += rawText(n); }
    return s;
  }
  function collapse(text, whiteSpace) {
    if (whiteSpace === 'pre' || whiteSpace === 'pre-wrap' || whiteSpace === 'break-spaces') {
      return text.replace(/\u2028/g, '\n');
    }
    if (whiteSpace === 'pre-line') {
      text = text.replace(/[ \t\f\r]+/g, ' ');
    } else {
      text = text.replace(/[ \t\n\f\r]+/g, ' ');
    }
    return text.replace(/\u2028/g, '\n').split('\n').map(function (l) { return l.trim(); }).join('\n').trim();
  }
  function rangeInfo(range) {
    var rects = Array.prototype.slice.call(range.getClientRects()).filter(function (r) { return r.width > 0 && r.height > 0; });
    var tops = [];
    var lineHeights = [];
    rects.forEach(function (r) {
      if (!tops.some(function (t) { return Math.abs(t - r.top) < 2; })) { tops.push(r.top); lineHeights.push(r.height); }
    });
    var b = range.getBoundingClientRect();
    return { rect: { x: round(b.left), y: round(b.top), w: round(b.width), h: round(b.height) }, lines: Math.max(tops.length, 1),
             contentHeight: round(lineHeights.length ? Math.max.apply(null, lineHeights) : b.height) };
  }
  function mixedStyles(el, cs) {
    var mixed = false;
    el.querySelectorAll('*').forEach(function (c) {
      if (c.tagName === 'BR') { return; }
      var s = getComputedStyle(c);
      if (s.fontWeight !== cs.fontWeight || s.fontStyle !== cs.fontStyle || s.fontSize !== cs.fontSize || s.color !== cs.color ||
          s.fontFamily !== cs.fontFamily || s.textDecorationLine !== cs.textDecorationLine) { mixed = true; }
    });
    return mixed;
  }
  function textOfLeaf(el, cs) {
    var range = document.createRange();
    range.selectNodeContents(el);
    var info = rangeInfo(range);
    info.content = collapse(rawText(el), cs.whiteSpace);
    info.mixed = mixedStyles(el, cs);
    return info;
  }

  // ---------------------------------------------------------------------------------------------------------
  // Sizing probes (flex items): change one property, see whether the box changes, restore

  function probe(el, props) {
    var saved = el.getAttribute('style');
    var before = el.getBoundingClientRect();
    for (var k in props) { el.style.setProperty(k, props[k], 'important'); }
    var after = el.getBoundingClientRect();
    if (saved === null) { el.removeAttribute('style'); } else { el.setAttribute('style', saved); }
    return { dw: Math.abs(after.width - before.width), dh: Math.abs(after.height - before.height) };
  }
  /** Specified width / height / flex-basis: a display:none element reports computed values ('auto' stays 'auto'). */
  function specified(el) {
    var saved = el.getAttribute('style');
    el.style.setProperty('display', 'none', 'important');
    var cs = getComputedStyle(el);
    var out = { width: cs.width, height: cs.height, flexBasis: cs.flexBasis };
    if (saved === null) { el.removeAttribute('style'); } else { el.setAttribute('style', saved); }
    return out;
  }
  function flexSizing(el, cs, parentCs) {
    var row = parentCs.flexDirection.indexOf('row') === 0;
    var main = row ? 'width' : 'height';
    var cross = row ? 'height' : 'width';
    var dMain = row ? 'dw' : 'dh';
    var dCross = row ? 'dh' : 'dw';
    var spec = specified(el);
    var sizing = {};
    var method = 'specified';
    if (parseFloat(cs.flexGrow) > 0) {
      sizing[main] = 'fill';
    } else if (spec[main] === 'auto' || /px|%/.test(spec[main])) {
      var basis = spec.flexBasis;
      sizing[main] = (basis !== 'auto' && basis !== 'content') || spec[main] !== 'auto' ? 'fixed' : 'hug';
    } else {
      // Fallback: does setting the size to auto change the box?
      method = 'probe';
      var p = {}; p[main] = 'auto'; p['flex-basis'] = 'auto'; p['min-' + main] = 'auto'; p['max-' + main] = 'none';
      sizing[main] = probe(el, p)[dMain] > 0.5 ? 'fixed' : 'hug';
    }
    var align = cs.alignSelf === 'auto' ? parentCs.alignItems : cs.alignSelf;
    var stretchable = align === 'normal' || align === 'stretch';
    if (spec[cross] === 'auto') {
      // CSS stretches only items whose cross size is auto
      sizing[cross] = stretchable ? 'fill' : 'hug';
    } else if (/px|%/.test(spec[cross])) {
      sizing[cross] = 'fixed';
    } else {
      method = 'probe';
      var stretched = stretchable && probe(el, { 'align-self': 'flex-start' })[dCross] > 0.5;
      var q = { 'align-self': 'flex-start' }; q[cross] = 'auto'; q['min-' + cross] = 'auto'; q['max-' + cross] = 'none';
      sizing[cross] = stretched ? 'fill' : (probe(el, q)[dCross] > 0.5 ? 'fixed' : 'hug');
    }
    return { h: sizing.width, v: sizing.height, align: align, method: method };
  }

  // ---------------------------------------------------------------------------------------------------------
  // Colour tokens: which custom property a colour came from (last matching rule wins; verified in Python)

  var colorRules = [];
  var rootTokens = {};
  function collectRules(rules) {
    for (var i = 0; i < rules.length; i++) {
      var rule = rules[i];
      if (rule.cssRules && !rule.selectorText) { try { collectRules(rule.cssRules); } catch (e) {} continue; }
      if (!rule.selectorText || !rule.style) { continue; }
      for (var j = 0; j < rule.style.length; j++) {
        var name = rule.style[j];
        if (name.indexOf('--') === 0 && /^(:root|html)$/.test(rule.selectorText.trim())) {
          rootTokens[name] = getComputedStyle(document.documentElement).getPropertyValue(name).trim();
        }
      }
      var refs = {};
      var any = false;
      COLOR_PROPS.forEach(function (p) {
        var m = /^\s*var\(\s*(--[\w-]+)\s*\)\s*$/.exec(rule.style.getPropertyValue(p));
        if (m) { refs[p] = m[1]; any = true; }
      });
      if (any) { colorRules.push({ selector: rule.selectorText, refs: refs }); }
    }
  }
  function tokenRefs(el) {
    var out = {};
    colorRules.forEach(function (r) {
      var ok = false;
      try { ok = el.matches(r.selector); } catch (e) {}
      if (ok) { for (var k in r.refs) { out[k] = r.refs[k]; } }
    });
    COLOR_PROPS.forEach(function (p) {
      var m = /^\s*var\(\s*(--[\w-]+)\s*\)\s*$/.exec(el.style.getPropertyValue(p));
      if (m) { out[p] = m[1]; }
    });
    var res = {};
    if (out['background-color'] || out['background']) { res.background = out['background-color'] || out['background']; }
    if (out['color']) { res.color = out['color']; }
    if (out['border-color'] || out['border-top-color'] || out['border']) { res.border = out['border-color'] || out['border-top-color'] || out['border']; }
    return res;
  }

  // ---------------------------------------------------------------------------------------------------------
  // Walk

  function walk(el, parentCs) {
    var cs = getComputedStyle(el);
    if (cs.display === 'none' || cs.display === 'contents' && false) { return null; }
    var node = { kind: 'element', tag: el.tagName.toLowerCase(), attrs: attrsOf(el), path: domPath(el), rect: rectOf(el), style: pickStyle(cs) };
    var t = transforms.get(el);
    if (t) { node.transform = { matrix: t.matrix, origin: t.origin, cx: round(t.cx), cy: round(t.cy) }; }
    var refs = tokenRefs(el);
    if (Object.keys(refs).length) { node.tokens = refs; }

    var inFlex = parentCs && /flex$/.test(parentCs.display) && cs.position !== 'absolute' && cs.position !== 'fixed';
    if (inFlex) { node.sizing = flexSizing(el, cs, parentCs); }

    ['::before', '::after'].forEach(function (pseudo) {
      var c = getComputedStyle(el, pseudo).content;
      if (c && c !== 'none' && c !== 'normal') { node.pseudo = (node.pseudo || []).concat([pseudo]); }
    });

    if (el.tagName === 'IMG') {
      node.image = { src: el.currentSrc || el.src, naturalWidth: el.naturalWidth, naturalHeight: el.naturalHeight };
      return node;
    }
    if (REPLACED[el.tagName]) {
      node.replaced = true;
      return node;
    }

    var flowContainer = /(flex|grid)$/.test(cs.display);
    if (!flowContainer && isTextLeaf(el)) {
      node.text = textOfLeaf(el, cs);
      return node;
    }

    node.children = [];
    for (var n = el.firstChild; n; n = n.nextSibling) {
      if (n.nodeType === 1) {
        if (SKIP[n.tagName] || n.id === 'dwocanvas-output') { continue; }
        var child = walk(n, cs);
        if (child) { node.children.push(child); }
      } else if (n.nodeType === 3 && /\S/.test(n.data)) {
        // Text run beside elements (anonymous flex item / anonymous block): its own text node
        var range = document.createRange();
        range.selectNode(n);
        var info = rangeInfo(range);
        info.content = collapse(n.data, cs.whiteSpace);
        if (info.content) { node.children.push({ kind: 'text', text: info, path: domPath(el) + '#' + node.children.length }); }
      }
    }
    return node;
  }

  function extract() {
    try { collectRules(document.styleSheets ? Array.prototype.reduce.call(document.styleSheets, function (acc, s) {
      try { return acc.concat(Array.prototype.slice.call(s.cssRules)); } catch (e) { warnings.push('Stylesheet not readable: ' + (s.href || 'inline')); return acc; }
    }, []) : []); } catch (e) { warnings.push('Token scan failed: ' + e); }
    freezeTransforms();
    var html = getComputedStyle(document.documentElement);
    var out = {
      adapter: 'dwocanvas-html-extractor/1',
      title: document.title,
      viewport: { w: window.innerWidth, h: window.innerHeight },
      canvas: { html: html.backgroundColor, htmlImage: html.backgroundImage },
      fonts: [],
      tokens: rootTokens,
      warnings: warnings,
      body: walk(document.body, null)
    };
    if (document.fonts) {
      document.fonts.forEach(function (f) { out.fonts.push({ family: f.family, weight: f.weight, style: f.style, status: f.status }); });
    }
    return out;
  }

  function deliver(data) {
    var json = JSON.stringify(data);
    if (window.parent && window.parent !== window) {
      window.parent.postMessage({ dwocanvas: json }, '*');
    } else {
      var pre = document.createElement('pre');
      pre.id = 'dwocanvas-output';
      pre.style.display = 'none';
      pre.textContent = json;
      document.body.appendChild(pre);
    }
  }

  function whenReady() {
    var waits = [];
    if (document.fonts && document.fonts.ready) { waits.push(document.fonts.ready); }
    Array.prototype.forEach.call(document.images, function (img) {
      if (!img.complete) { waits.push(new Promise(function (r) { img.addEventListener('load', r); img.addEventListener('error', r); })); }
    });
    return Promise.all(waits).then(function () { return new Promise(function (r) { setTimeout(r, 50); }); });
  }

  function run() {
    whenReady().then(function () {
      var data;
      try { data = extract(); } catch (e) { data = { error: String(e && e.stack || e) }; }
      deliver(data);
    });
  }
  if (document.readyState === 'complete') { run(); } else { window.addEventListener('load', run); }
})();

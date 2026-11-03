"""dwo-Lens analyzer: raw extractor JSON -> unified graph, SQLite index, diagrams, gate findings, HTML report.

Standard library only. Deterministic: the same raw input always produces the same output.

    python lens_analyze.py --raw <raw dir> --project <x.uproject> --out <out dir> [--run-id <id>]
"""
from __future__ import annotations

import argparse
import html
import json
import re
import sqlite3
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lens_quality  # noqa: E402

SCHEMA = "lens.graph/0.1"
EXTRACTOR_VERSION = "lens-analyze/0.1"
MAX_DIAGRAM_NODES = 120
MAX_FLOW_NODES = 60
MAX_REPORT_FLOWS = 60
TICK_EVENTS = {"ReceiveTick", "Tick"}
EXTERNAL_PREFIXES = ("__ExternalActors__", "__ExternalObjects__")


# --------------------------------------------------------------------------- helpers

def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def normalize_class(path: str) -> str:
    """'/Game/X.SKEL_X_C' -> '/Game/X.X_C' (skeleton classes are an editor artifact)."""
    if not path:
        return path
    head, _, tail = path.rpartition(".")
    if tail.startswith("SKEL_"):
        return f"{head}.{tail[5:]}"
    return path


def short_name(path: str) -> str:
    name = path.rsplit(".", 1)[-1] if "." in path else path.rsplit("/", 1)[-1]
    return name.split(":")[-1]


def collapse_package(package: str, roots: list[str]) -> str:
    """World Partition external actor/object packages belong to their map."""
    for root in roots:
        for prefix in EXTERNAL_PREFIXES:
            marker = f"{root}/{prefix}/"
            if package.startswith(marker):
                rest = package[len(marker):]
                # /Game/__ExternalActors__/<map path...>/<x>/<yy>/<hash>
                parts = rest.split("/")
                return f"{root}/" + "/".join(parts[:-3]) if len(parts) > 3 else package
    return package


def under_roots(package: str, roots: list[str]) -> bool:
    return any(package == r or package.startswith(r + "/") for r in roots)


def mermaid_id(key: str) -> str:
    return "n_" + re.sub(r"[^A-Za-z0-9_]", "_", key)


def mermaid_label(text: str) -> str:
    return text.replace("\n", " ").replace('"', "#quot;").replace("<", "#lt;").replace(">", "#gt;")


def tarjan(graph: dict[str, set[str]]) -> list[list[str]]:
    """Strongly connected components with more than one node (cycles), sorted."""
    return [sorted(c) for c in strongly_connected(graph) if len(c) > 1]


def reach_counts(graph: dict[str, set[str]], nodes) -> dict[str, int]:
    """Number of other nodes reachable from each node. Cycles are condensed first and reachability is
    propagated as integer bitsets in reverse topological order, so large asset graphs stay fast."""
    all_nodes = set(nodes) | set(graph)
    for deps in graph.values():
        all_nodes |= deps
    bit = {n: 1 << i for i, n in enumerate(sorted(all_nodes))}
    comp_of, reach = {}, []
    for ci, comp in enumerate(strongly_connected({n: graph.get(n, set()) for n in all_nodes})):
        mask = 0
        for n in comp:
            comp_of[n] = ci
            mask |= bit[n]
        for n in comp:  # components arrive sinks first, so successors are already final
            for d in graph.get(n, ()):
                dc = comp_of[d]
                if dc != ci:
                    mask |= reach[dc]
        reach.append(mask)
    return {n: bin(reach[comp_of[n]]).count("1") - (0 if len(graph.get(n, ())) and n in graph.get(n, ()) else 1)
            for n in nodes}


def strongly_connected(graph: dict[str, set[str]]) -> list[list[str]]:
    """All strongly connected components in reverse topological order (sinks first). Iterative, so
    reference chains of any length cannot hit Python's recursion limit."""
    index, low, stack, on_stack, result = {}, {}, [], set(), []
    counter = 0
    for root in sorted(graph):
        if root in index:
            continue
        work = [(root, iter(sorted(graph.get(root, ()))))]
        index[root] = low[root] = counter
        counter += 1
        stack.append(root)
        on_stack.add(root)
        while work:
            v, children = work[-1]
            advanced = False
            for w in children:
                if w not in index:
                    index[w] = low[w] = counter
                    counter += 1
                    stack.append(w)
                    on_stack.add(w)
                    work.append((w, iter(sorted(graph.get(w, ())))))
                    advanced = True
                    break
                if w in on_stack:
                    low[v] = min(low[v], index[w])
            if advanced:
                continue
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[v])
            if low[v] == index[v]:
                component = []
                while True:
                    w = stack.pop()
                    on_stack.discard(w)
                    component.append(w)
                    if w == v:
                        break
                result.append(component)
    return result


# --------------------------------------------------------------------------- graph model

class Graph:
    def __init__(self):
        self.symbols: dict[str, dict] = {}
        self.relations: dict[tuple, dict] = {}

    def symbol(self, key: str, kind: str, name: str, external: bool = False, **extra) -> str:
        if key not in self.symbols:
            self.symbols[key] = {"key": key, "kind": kind, "name": name, "external": external, **extra}
        else:
            # A project definition wins over an earlier external placeholder.
            if not external and self.symbols[key]["external"]:
                self.symbols[key].update({"kind": kind, "name": name, "external": False, **extra})
        return key

    def relate(self, source: str, target: str, rel: str, evidence: str, status: str = "verified", confidence: float = 1.0):
        if source == target:
            return
        k = (source, target, rel)
        entry = self.relations.setdefault(k, {
            "source": source, "target": target, "type": rel, "status": status,
            "confidence": confidence, "evidence": [], "count": 0,
        })
        entry["count"] += 1
        if len(entry["evidence"]) < 8 and evidence not in entry["evidence"]:
            entry["evidence"].append(evidence)

    def edges(self, *types: str):
        return [r for r in self.relations.values() if not types or r["type"] in types]


# --------------------------------------------------------------------------- module scan

def scan_modules(project_file: Path, g: Graph) -> dict[str, set[str]]:
    root = project_file.parent
    deps: dict[str, set[str]] = defaultdict(set)
    uproject = load_json(project_file)
    project_modules = {m["Name"] for m in uproject.get("Modules", [])}

    build_files = list((root / "Source").rglob("*.Build.cs")) if (root / "Source").exists() else []
    plugins_dir = root / "Plugins"
    plugin_files = []
    if plugins_dir.exists():
        plugin_files = [p for p in plugins_dir.rglob("*.uplugin") if "__DwoLensInspector" not in p.parts]
        build_files += [p for p in plugins_dir.rglob("*.Build.cs") if "__DwoLensInspector" not in p.parts]

    for plugin in plugin_files:
        desc = load_json(plugin)
        pkey = g.symbol(f"plugin:{plugin.stem}", "plugin", plugin.stem, file=str(plugin.relative_to(root)))
        for module in desc.get("Modules", []):
            project_modules.add(module["Name"])
            g.relate(pkey, f"module:{module['Name']}", "Contains", str(plugin.relative_to(root)))
        for dep in desc.get("Plugins", []):
            if dep.get("Enabled", True):
                g.symbol(f"plugin:{dep['Name']}", "plugin", dep["Name"], external=True)
                g.relate(pkey, f"plugin:{dep['Name']}", "DependsOnPlugin", str(plugin.relative_to(root)))

    pattern = re.compile(r"(Public|Private)DependencyModuleNames\s*\.\s*Add(?:Range)?\s*\((.*?)\)\s*;", re.S)
    for build in build_files:
        module = build.name[: -len(".Build.cs")]
        project_modules.add(module)
        text = re.sub(r"//.*", "", build.read_text(encoding="utf-8", errors="replace"))
        for visibility, body in pattern.findall(text):
            for dep in re.findall(r'"([A-Za-z0-9_]+)"', body):
                deps[module].add(dep)
                g.relate(f"module:{module}", f"module:{dep}", "DependsOnModule",
                         f"{build.relative_to(root)} ({visibility})")

    for module in project_modules:
        g.symbol(f"module:{module}", "module", module)
    for module, targets in deps.items():
        for dep in targets:
            g.symbol(f"module:{dep}", "module", dep, external=dep not in project_modules)
    return deps


# --------------------------------------------------------------------------- build graph

def build(raw: Path, project_file: Path):
    meta = load_json(raw / "meta.json")
    reflection = load_json(raw / "reflection.json")
    blueprints = load_json(raw / "blueprints.json")["blueprints"]
    assets = load_json(raw / "assets.json")["assets"]
    roots = meta.get("contentRoots", ["/Game"])

    g = Graph()
    module_deps = scan_modules(project_file, g)

    bp_classes = {normalize_class(c["path"]) for c in reflection["blueprint"]}
    native_classes = {c["path"] for c in reflection["native"]}

    def class_key(path: str) -> str:
        path = normalize_class(path)
        if path in bp_classes:
            return f"bp:{path}"
        key = f"uclass:{path}"
        g.symbol(key, "class", short_name(path), external=path not in native_classes,
                 module=path.split(".")[0].replace("/Script/", "") if path.startswith("/Script/") else None)
        return key

    # classes ---------------------------------------------------------------
    for origin in ("native", "blueprint"):
        for c in reflection[origin]:
            path = normalize_class(c["path"])
            key = f"bp:{path}" if origin == "blueprint" else f"uclass:{path}"
            module = path.split(".")[0].replace("/Script/", "") if origin == "native" else None
            g.symbol(key, "blueprint" if origin == "blueprint" else "class", c["name"], module=module,
                     package=c["package"], abstract=c.get("abstract", False),
                     functions=[f["name"] for f in c["functions"] if not f["name"].startswith("ExecuteUbergraph")],
                     properties=[{"name": p["name"], "type": p["cppType"], "replicated": p["replicated"]}
                                 for p in c["properties"] if p["name"] != "UberGraphFrame"])
            if c.get("super"):
                g.relate(key, class_key(c["super"]), "Inherits", c["path"])
            for iface in c.get("interfaces", []):
                g.relate(key, class_key(iface), "Implements", c["path"])
            for prop in c["properties"]:
                for ref in prop.get("refs", []):
                    target = ref["target"]
                    if ref["kind"] == "struct":
                        continue
                    if ref["kind"] == "delegate":
                        continue
                    rel = {"hard": "RefHard", "soft": "RefSoft", "weak": "RefWeak", "interface": "RefInterface"}[ref["kind"]]
                    g.relate(key, class_key(target), rel, f"{c['name']}.{prop['name']}")
            if module:
                g.relate(f"module:{module}", key, "Contains", c["package"])

    # blueprint graphs -------------------------------------------------------
    bp_index = {}
    tick_users = []
    unknown_calls = 0
    for bp in blueprints:
        bkey = f"bp:{normalize_class(bp['generatedClass'])}"
        g.symbol(bkey, "blueprint", short_name(bp["asset"]), package=bp["package"], asset_class=bp["assetClass"])
        g.symbols[bkey]["asset_class"] = bp["assetClass"]
        bp_index[bkey] = bp
        for graph in bp["graphs"]:
            exec_out = defaultdict(int)
            for link in graph["links"]:
                if link["kind"] == "exec":
                    exec_out[link["from"]] += 1
            for node in graph["nodes"]:
                role = node.get("role")
                owner = normalize_class(node.get("memberOwner") or "")
                where = f"{graph['name']}#{node['id'][:8]} {node.get('member') or node['title']}"
                if role == "call":
                    if owner:
                        g.relate(bkey, class_key(owner), "Calls", where)
                    else:
                        unknown_calls += 1
                        g.symbol("unknown:call", "unknown", "unresolved call", external=True)
                        g.relate(bkey, "unknown:call", "Calls", where, status="unknown", confidence=0.0)
                elif role == "delegate" and owner:
                    rel = "BindsDelegate" if "Add" in node["class"] or "Assign" in node["class"] else "UsesDelegate"
                    g.relate(bkey, class_key(owner), rel, where)
                elif role == "cast" and node.get("member"):
                    g.relate(bkey, class_key(node["member"]), "CastsTo", where)
                elif role == "event" and node.get("member") in TICK_EVENTS and exec_out.get(node["id"], 0) > 0:
                    tick_users.append({"blueprint": short_name(bp["asset"]), "graph": graph["name"], "node": node["id"]})

    # assets ----------------------------------------------------------------
    asset_hard: dict[str, set[str]] = defaultdict(set)
    asset_soft: dict[str, set[str]] = defaultdict(set)
    asset_class: dict[str, str] = {}
    for a in assets:
        pkg = collapse_package(a["package"], roots)
        if pkg != a["package"]:
            continue  # folded into its map
        # A Blueprint package also holds its generated class; keep the Blueprint asset class.
        if pkg not in asset_class or asset_class[pkg].endswith("GeneratedClass"):
            asset_class[pkg] = a["class"]
    for a in assets:
        src = collapse_package(a["package"], roots)
        if src not in asset_class:
            continue
        for kind, bucket in (("hard", asset_hard), ("soft", asset_soft)):
            for dep in a[kind]:
                dep_pkg = collapse_package(dep, roots)
                if dep_pkg == src:
                    continue
                if dep_pkg.startswith("/Script/"):
                    if kind == "hard":
                        mod = dep_pkg[len("/Script/"):]
                        g.symbol(f"module:{mod}", "module", mod, external=True)
                        g.relate(f"asset:{src}", f"module:{mod}", "AssetUsesModule", a["package"])
                    continue
                if under_roots(dep_pkg, roots):
                    bucket[src].add(dep_pkg)
    for pkg, cls in asset_class.items():
        g.symbol(f"asset:{pkg}", "asset", pkg.rsplit("/", 1)[-1], asset_class=cls, package=pkg)
    for src, deps in asset_hard.items():
        for dep in deps:
            if dep in asset_class:
                g.relate(f"asset:{src}", f"asset:{dep}", "AssetHardRef", src)
    for src, deps in asset_soft.items():
        for dep in deps:
            if dep in asset_class and dep not in asset_hard.get(src, set()):
                g.relate(f"asset:{src}", f"asset:{dep}", "AssetSoftRef", src)

    context = {
        "meta": meta, "roots": roots, "module_deps": module_deps, "bp_index": bp_index,
        "tick_users": tick_users, "unknown_calls": unknown_calls,
        "asset_hard": asset_hard, "asset_soft": asset_soft, "asset_class": asset_class,
        "project_file": project_file,
    }
    return g, context


# --------------------------------------------------------------------------- checks

def run_checks(g: Graph, ctx: dict) -> dict:
    findings = []

    def add(severity: str, rule: str, message: str, items=None):
        findings.append({"severity": severity, "rule": rule, "message": message, "items": items or []})

    module_graph = {m: set(d) for m, d in ctx["module_deps"].items()}
    for cycle in tarjan(module_graph):
        add("fail", "module-cycle", f"모듈 순환 의존: {' ↔ '.join(cycle)}", cycle)

    hard = {k: set(v) for k, v in ctx["asset_hard"].items()}
    for cycle in tarjan(hard):
        add("warn", "asset-hard-cycle", f"에셋 하드 참조 순환 ({len(cycle)}개)", cycle)

    failures = ctx["meta"].get("blueprintLoadFailures", 0)
    if failures:
        add("warn", "bp-load-failure", f"로드하지 못한 Blueprint {failures}개")

    for t in ctx["tick_users"]:
        add("warn", "tick-usage", f"{t['blueprint']}: Tick 이벤트에 로직이 연결되어 있음 ({t['graph']})", [t])

    # Transitive hard-reference load chain per asset.
    loads = reach_counts(hard, ctx["asset_class"].keys())
    chains = sorted(loads.items(), key=lambda x: (-x[1], x[0]))[:10]
    heavy = [{"asset": p, "loads": n} for p, n in chains if n > 0]
    if heavy:
        add("info", "hard-ref-chain", "하드 참조로 함께 로드되는 에셋 수 상위", heavy)

    rels = g.edges()
    unknown = [r for r in rels if r["status"] == "unknown"]
    total = len(rels) or 1
    ratio = sum(r["count"] for r in unknown) / max(1, sum(r["count"] for r in rels))
    if ratio > 0.10:
        add("warn", "unknown-ratio", f"미확정 관계 비율 {ratio:.0%} (기준 10%)")

    replicated = [(s["name"], p["name"]) for s in g.symbols.values() if not s["external"]
                  for p in s.get("properties", []) if p.get("replicated")]
    if replicated:
        add("info", "replication", f"복제 프로퍼티 {len(replicated)}개", [f"{a}.{b}" for a, b in replicated])

    counts = defaultdict(int)
    for r in rels:
        counts[r["status"]] += r["count"]
    return {"verdict": verdict_of(findings), "findings": findings, "relationStatus": dict(counts),
            "unknownRatio": round(ratio, 4)}


def verdict_of(findings: list) -> str:
    if any(f["severity"] == "fail" for f in findings):
        return "fail"
    if any(f["severity"] == "warn" for f in findings):
        return "warn"
    return "pass"


# --------------------------------------------------------------------------- outputs

def write_sqlite(g: Graph, path: Path, meta: dict):
    if path.exists():
        path.unlink()
    db = sqlite3.connect(path)
    db.executescript("""
        CREATE TABLE snapshot (id INTEGER PRIMARY KEY, engine TEXT, extracted_at TEXT);
        CREATE TABLE symbol (id INTEGER PRIMARY KEY, symbol_key TEXT UNIQUE NOT NULL, kind TEXT NOT NULL,
                             name TEXT NOT NULL, is_external INTEGER NOT NULL, attrs TEXT);
        CREATE TABLE relation (id INTEGER PRIMARY KEY, source_key TEXT NOT NULL, target_key TEXT NOT NULL,
                               relation_type TEXT NOT NULL, status TEXT NOT NULL, confidence REAL NOT NULL,
                               evidence TEXT NOT NULL, count INTEGER NOT NULL, extractor_version TEXT NOT NULL);
        CREATE INDEX ix_rel_source ON relation(source_key, relation_type);
        CREATE INDEX ix_rel_target ON relation(target_key, relation_type);
    """)
    db.execute("INSERT INTO snapshot(engine, extracted_at) VALUES (?, ?)", (meta.get("engineVersion"), meta.get("extractedAtUtc")))
    db.executemany("INSERT INTO symbol(symbol_key, kind, name, is_external, attrs) VALUES (?,?,?,?,?)",
                   ((s["key"], s["kind"], s["name"], int(s["external"]),
                     json.dumps({k: v for k, v in s.items() if k not in ("key", "kind", "name", "external")}, ensure_ascii=False))
                    for s in g.symbols.values()))
    db.executemany("INSERT INTO relation(source_key, target_key, relation_type, status, confidence, evidence, count, extractor_version)"
                   " VALUES (?,?,?,?,?,?,?,?)",
                   ((r["source"], r["target"], r["type"], r["status"], r["confidence"],
                     json.dumps(r["evidence"], ensure_ascii=False), r["count"], EXTRACTOR_VERSION)
                    for r in g.relations.values()))
    db.commit()
    db.close()


def class_diagram(g: Graph) -> tuple[str, str]:
    """Project classes and blueprints plus their direct external parents (collapsed)."""
    project = [s for s in g.symbols.values() if s["kind"] in ("class", "blueprint") and not s["external"]]
    keys = {s["key"] for s in project}
    edges = [r for r in g.edges("Inherits", "Implements", "RefHard", "RefSoft") if r["source"] in keys]
    shown = set(keys) | {r["target"] for r in edges if r["type"] in ("Inherits", "Implements")}
    shown |= {r["target"] for r in edges if r["target"] in keys}
    shown = set(sorted(shown)[:MAX_DIAGRAM_NODES])

    # Readable, unique diagram ids derived from class names.
    ids: dict[str, str] = {}
    used: set[str] = set()
    for key in sorted(shown):
        base = re.sub(r"[^A-Za-z0-9_]", "_", g.symbols.get(key, {"name": short_name(key)})["name"]) or "C"
        cid, n = base, 2
        while cid in used:
            cid, n = f"{base}_{n}", n + 1
        used.add(cid)
        ids[key] = cid

    mmd = ["classDiagram"]
    puml = ["@startuml", "hide empty members", "skinparam classAttributeIconSize 0"]
    for key in sorted(shown):
        s = g.symbols.get(key, {"name": short_name(key), "external": True})
        cid = ids[key]
        label = s["name"]
        if s["external"]:
            mmd.append(f"class {cid}")
            mmd.append(f"<<external>> {cid}")
            puml.append(f'class "{label}" as {cid} <<external>>')
            continue
        members = [f"+{p['type'].replace('<', '~').replace('>', '~')} {p['name']}" for p in s.get("properties", [])[:12]]
        methods = [f"+{f}()" for f in s.get("functions", [])[:12]]
        mmd.append(f"class {cid} {{")
        mmd += [f"  {m}" for m in members + methods]
        mmd.append("}")
        stereo = " <<blueprint>>" if s["kind"] == "blueprint" else ""
        puml.append(f'class "{label}" as {cid}{stereo} {{')
        puml += [f"  {m}" for m in members + methods]
        puml.append("}")
    arrows = {"Inherits": ("<|--", "<|--"), "Implements": ("<|..", "<|.."), "RefHard": ("<--", "<--"), "RefSoft": ("<..", "<..")}
    for r in sorted(edges, key=lambda r: (r["source"], r["target"], r["type"])):
        if r["source"] in shown and r["target"] in shown:
            m, p = arrows[r["type"]]
            label = "" if r["type"] in ("Inherits", "Implements") else f" : {r['type']}"
            mmd.append(f"{ids[r['target']]} {m} {ids[r['source']]}{label}")
            puml.append(f"{ids[r['target']]} {p} {ids[r['source']]}{label}")
    puml.append("@enduml")
    return "\n".join(mmd), "\n".join(puml)


def module_diagram(g: Graph) -> str | None:
    edges = g.edges("DependsOnModule", "DependsOnPlugin")
    if not edges:
        return None
    lines = ["flowchart LR"]
    for r in sorted(edges, key=lambda r: (r["source"], r["target"])):
        for key in (r["source"], r["target"]):
            s = g.symbols.get(key, {"name": key, "external": True})
            shape = f'(["{mermaid_label(s["name"])}"])' if s["external"] else f'["{mermaid_label(s["name"])}"]'
            lines.append(f"{mermaid_id(key)}{shape}")
        lines.append(f"{mermaid_id(r['source'])} --> {mermaid_id(r['target'])}")
    return "\n".join(dict.fromkeys(lines))


def asset_diagram(g: Graph) -> str | None:
    edges = g.edges("AssetHardRef", "AssetSoftRef")
    if not edges:
        return None
    nodes = sorted({r["source"] for r in edges} | {r["target"] for r in edges})[:MAX_DIAGRAM_NODES]
    keep = set(nodes)
    lines = ["flowchart LR"]
    for key in nodes:
        s = g.symbols[key]
        lines.append(f'{mermaid_id(key)}["{mermaid_label(s["name"])}<br/><small>{mermaid_label(s.get("asset_class", "").rsplit(".", 1)[-1])}</small>"]')
    for r in sorted(edges, key=lambda r: (r["source"], r["target"])):
        if r["source"] in keep and r["target"] in keep:
            arrow = "-->" if r["type"] == "AssetHardRef" else "-.->"
            lines.append(f"{mermaid_id(r['source'])} {arrow} {mermaid_id(r['target'])}")
    return "\n".join(lines)


def flow_diagrams(bp: dict) -> list[tuple[str, str]]:
    """Exec flow from every event / function entry, one diagram per graph."""
    out = []
    for graph in bp["graphs"]:
        nodes = {n["id"]: n for n in graph["nodes"]}
        exec_links = [l for l in graph["links"] if l["kind"] == "exec"]
        if not exec_links:
            continue
        adj = defaultdict(list)
        for l in exec_links:
            adj[l["from"]].append(l)
        starts = [n["id"] for n in graph["nodes"]
                  if (n.get("role") == "event" or n["class"] == "K2Node_FunctionEntry") and adj.get(n["id"])]
        seen, order, edges = set(), [], []
        stack = list(reversed(starts))
        while stack and len(order) < MAX_FLOW_NODES:
            nid = stack.pop()
            if nid in seen or nid not in nodes:
                continue
            seen.add(nid)
            order.append(nid)
            for l in reversed(adj.get(nid, [])):
                edges.append(l)
                stack.append(l["to"])
        if len(order) < 2:
            continue
        lines = ["flowchart TD"]
        for nid in order:
            n = nodes[nid]
            label = mermaid_label(n["title"])
            role = n.get("role")
            if role == "event" or n["class"] == "K2Node_FunctionEntry":
                lines.append(f'{mermaid_id(nid)}(["{label}"])')
            elif role == "call":
                lines.append(f'{mermaid_id(nid)}["{label}"]')
            else:
                lines.append(f'{mermaid_id(nid)}{{{{"{label}"}}}}' if "Branch" in n["class"] or "IfThen" in n["class"] else f'{mermaid_id(nid)}["{label}"]')
        for l in edges:
            if l["from"] in seen and l["to"] in seen:
                pin = l["fromPin"]
                label = "" if pin in ("then", "OutputDelegate", "execute") else f"|{mermaid_label(pin)}|"
                lines.append(f"{mermaid_id(l['from'])} -->{label} {mermaid_id(l['to'])}")
        out.append((graph["name"], "\n".join(lines)))
    return out


# --------------------------------------------------------------------------- report

REPORT_CSS = """
:root{--bg:#f6f8f9;--sheet:#fff;--ink:#16212b;--ink2:#4a5a68;--ink3:#7b8a97;--rule:#d9e0e5;--det:#0f6e66;--detbg:#e3f3f1;
--warn:#a35c00;--warnbg:#fdf0dc;--fail:#b3261e;--failbg:#fde7e5;--info:#1d5fa8;--infobg:#e4eefa}
@media (prefers-color-scheme:dark){:root{--bg:#0f151a;--sheet:#151d24;--ink:#e6edf2;--ink2:#a9b7c2;--ink3:#7d8c98;--rule:#2a3742;
--det:#5fc9bd;--detbg:#12312e;--warn:#f0a557;--warnbg:#3a2711;--fail:#f28b82;--failbg:#3b1715;--info:#7fb1ea;--infobg:#172a40;color-scheme:dark}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.65 "Malgun Gothic","Apple SD Gothic Neo",sans-serif;
padding:28px max(16px,4vw)}main{max-width:1100px;margin:0 auto;display:grid;gap:20px}
section{background:var(--sheet);border:1px solid var(--rule);border-radius:6px;padding:22px 26px;min-width:0}
h1{font-size:26px;margin:0}h2{font-size:18px;margin:0 0 12px}h3{font-size:15px;margin:16px 0 8px}
.meta{color:var(--ink3);font:12px Consolas,monospace}.kpis{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px;margin-top:14px}
.kpi{border:1px solid var(--rule);border-radius:6px;padding:10px 14px}.kpi b{display:block;font-size:22px}.kpi span{color:var(--ink3);font-size:12px}
.badge{display:inline-block;padding:2px 10px;border-radius:3px;font:12px Consolas,monospace;text-transform:uppercase}
.pass{background:var(--detbg);color:var(--det)}.warn{background:var(--warnbg);color:var(--warn)}.fail{background:var(--failbg);color:var(--fail)}.info{background:var(--infobg);color:var(--info)}
table{width:100%;border-collapse:collapse;font-size:13.5px}th,td{text-align:left;padding:7px 10px;border-bottom:1px solid var(--rule);vertical-align:top}
th{color:var(--ink3);font:11px Consolas,monospace;text-transform:uppercase}.scroll{overflow-x:auto}
.mermaid{background:var(--sheet);overflow-x:auto}details{border-top:1px dashed var(--rule);padding-top:8px;margin-top:8px}
summary{cursor:pointer;color:var(--ink2)}code{font-family:Consolas,monospace;font-size:12.5px}
"""


def render_report(out: Path, run_id: str, g: Graph, ctx: dict, result: dict, diagrams: dict, quality_html: str = ""):
    meta = ctx["meta"]
    esc = html.escape
    project_syms = [s for s in g.symbols.values() if not s["external"]]
    kinds = defaultdict(int)
    for s in project_syms:
        kinds[s["kind"]] += 1
    status = result["relationStatus"]

    rows = []
    for f in result["findings"]:
        items = ""
        if f["items"]:
            preview = [i if isinstance(i, str) else json.dumps(i, ensure_ascii=False) for i in f["items"][:15]]
            items = "<details><summary>대상 {}건</summary><code>{}</code></details>".format(
                len(f["items"]), "<br>".join(esc(p) for p in preview))
        rows.append(f"<tr><td><span class='badge {f['severity']}'>{f['severity']}</span></td>"
                    f"<td><code>{esc(f['rule'])}</code></td><td>{esc(f['message'])}{items}</td></tr>")
    findings_html = "".join(rows) or "<tr><td colspan='3'>발견 사항 없음</td></tr>"

    def mermaid_block(title: str, source: str | None, note: str = "") -> str:
        if not source:
            return f"<h3>{esc(title)}</h3><p class='meta'>해당 데이터 없음</p>"
        return f"<h3>{esc(title)}</h3>{note}<pre class='mermaid'>{esc(source)}</pre>"

    # Large projects have thousands of graphs; the page keeps the biggest ones and links the rest.
    all_flows = [(bp_name, gname, src) for bp_name, graphs in diagrams["flows"].items() for gname, src in graphs]
    shown = sorted(all_flows, key=lambda f: -f[2].count("\n"))[:MAX_REPORT_FLOWS]
    shown_keys = {(b, gname) for b, gname, _ in shown}
    flows_html = []
    for bp_name, graphs in diagrams["flows"].items():
        inner = "".join(mermaid_block(f"{bp_name} · {gname}", src) for gname, src in graphs if (bp_name, gname) in shown_keys)
        if inner:
            flows_html.append(f"<details{' open' if len(shown) <= 12 else ''}><summary>{esc(bp_name)}</summary>{inner}</details>")
    if len(all_flows) > len(shown):
        flows_html.append(f"<p class='meta'>실행 흐름 {len(all_flows)}개 중 큰 {len(shown)}개만 표시했습니다. 전체는 flows/*.mmd 파일에 있습니다.</p>")

    calls_by_source = defaultdict(set)
    for r in g.edges("Calls"):
        calls_by_source[r["source"]].add(g.symbols.get(r["target"], {"name": r["target"]})["name"])
    bp_rows = []
    for key, bp in sorted(ctx["bp_index"].items()):
        targets = ", ".join(sorted(calls_by_source.get(key, ())))
        bp_rows.append(f"<tr><td>{esc(short_name(bp['asset']))}</td><td>{esc(bp['assetClass'])}</td>"
                       f"<td>{esc(short_name(bp['parent']))}</td><td>{len(bp['graphs'])}</td><td>{esc(targets)}</td></tr>")

    doc = f"""<!doctype html><html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>dwo-Lens 리포트 · {esc(Path(meta['project']).stem)}</title><style>{REPORT_CSS}</style></head><body><main>
<section><div class="meta">{esc(run_id)} · UE {esc(meta['engineVersion'].split('-')[0])} · {esc(meta['extractedAtUtc'])}</div>
<h1>{esc(Path(meta['project']).stem)} 아키텍처 리포트 <span class="badge {result['verdict']}">{result['verdict']}</span></h1>
<div class="kpis">
<div class="kpi"><b>{kinds['class']}</b><span>C++ 클래스</span></div>
<div class="kpi"><b>{kinds['blueprint']}</b><span>Blueprint</span></div>
<div class="kpi"><b>{kinds['asset']}</b><span>에셋 (외부 액터 접음)</span></div>
<div class="kpi"><b>{kinds['module']}</b><span>프로젝트 모듈</span></div>
<div class="kpi"><b>{status.get('verified', 0)}</b><span>verified 관계</span></div>
<div class="kpi"><b>{status.get('unknown', 0)}</b><span>unknown 관계</span></div>
</div></section>
<section><h2>검증 게이트</h2><div class="scroll"><table><thead><tr><th>판정</th><th>규칙</th><th>내용</th></tr></thead><tbody>{findings_html}</tbody></table></div></section>
{quality_html}
<section><h2>클래스 구조</h2>{mermaid_block('클래스 UML', diagrams['classes'])}</section>
<section><h2>모듈 의존성</h2>{mermaid_block('모듈 · 플러그인', diagrams['modules'])}</section>
<section><h2>에셋 참조</h2>{mermaid_block('에셋 참조 (실선 hard · 점선 soft)', diagrams['assets'])}</section>
<section><h2>Blueprint</h2><div class="scroll"><table><thead><tr><th>이름</th><th>종류</th><th>부모</th><th>그래프</th><th>호출 대상 클래스</th></tr></thead>
<tbody>{''.join(bp_rows) or "<tr><td colspan='5'>없음</td></tr>"}</tbody></table></div>
<h3>실행 흐름</h3>{''.join(flows_html) or "<p class='meta'>실행 흐름 없음</p>"}</section>
<section class="meta">산출물: graph.json · index.db · class_uml.puml · class_uml.mmd · findings.json · quality.json · statemonitor.json · flows/*.mmd</section>
</main>
<script src="https://cdn.jsdelivr.net/npm/mermaid@10.9.1/dist/mermaid.min.js"></script>
<script>mermaid.initialize({{startOnLoad:true,securityLevel:'strict',maxTextSize:200000,
theme:matchMedia('(prefers-color-scheme: dark)').matches?'dark':'neutral'}});</script>
</body></html>"""
    (out / "report.html").write_text(doc, encoding="utf-8")


# --------------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw", required=True, type=Path)
    ap.add_argument("--project", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--run-id", default="manual")
    ap.add_argument("--config", nargs="*", type=Path, default=[],
                    help="config JSON files merged in order (system default first, project override last)")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)

    g, ctx = build(args.raw, args.project)
    result = run_checks(g, ctx)
    cfg = lens_quality.load_config(args.config or [Path(__file__).resolve().parent.parent / "config" / "default.json"])
    quality_findings, quality = lens_quality.run(ctx, cfg)
    result["findings"] += quality_findings
    result["verdict"] = verdict_of(result["findings"])
    (args.out / "quality.json").write_text(json.dumps(quality, ensure_ascii=False, indent=1), encoding="utf-8")
    (args.out / "statemonitor.json").write_text(json.dumps(quality["stateMonitor"], ensure_ascii=False, indent=1), encoding="utf-8")

    classes_mmd, classes_puml = class_diagram(g)
    flows = {}
    flow_dir = args.out / "flows"
    flow_dir.mkdir(exist_ok=True)
    for key, bp in sorted(ctx["bp_index"].items()):
        name = short_name(bp["asset"])
        flows[name] = flow_diagrams(bp)
        for gname, src in flows[name]:
            (flow_dir / f"{name}.{re.sub(r'[^A-Za-z0-9_]', '_', gname)}.mmd").write_text(src, encoding="utf-8")
    diagrams = {"classes": classes_mmd, "modules": module_diagram(g), "assets": asset_diagram(g), "flows": flows}

    graph_doc = {
        "schema": SCHEMA, "extractor": EXTRACTOR_VERSION, "meta": ctx["meta"],
        "symbols": sorted(g.symbols.values(), key=lambda s: s["key"]),
        "relations": sorted(g.relations.values(), key=lambda r: (r["source"], r["target"], r["type"])),
    }
    (args.out / "graph.json").write_text(json.dumps(graph_doc, ensure_ascii=False, indent=1), encoding="utf-8")
    (args.out / "findings.json").write_text(json.dumps({"runId": args.run_id, **result}, ensure_ascii=False, indent=1), encoding="utf-8")
    (args.out / "class_uml.mmd").write_text(classes_mmd, encoding="utf-8")
    (args.out / "class_uml.puml").write_text(classes_puml, encoding="utf-8")
    if diagrams["modules"]:
        (args.out / "modules.mmd").write_text(diagrams["modules"], encoding="utf-8")
    if diagrams["assets"]:
        (args.out / "assets.mmd").write_text(diagrams["assets"], encoding="utf-8")
    write_sqlite(g, args.out / "index.db", ctx["meta"])
    render_report(args.out, args.run_id, g, ctx, result, diagrams, lens_quality.render_html(quality))

    print(f"[dwo-lens] symbols={len(g.symbols)} relations={len(g.relations)} verdict={result['verdict']} "
          f"findings={len(result['findings'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

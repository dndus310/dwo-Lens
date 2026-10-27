"""dwo-Assay quality checks on top of the extracted graph (ST-2).

- naming:      asset prefix / folder rules (UE5 style guide defaults, overridable in config)
- unused:      assets unreachable from maps and Config/*.ini, plus leftover redirectors
- complexity:  Blueprint graph size, branching and exec chain length
- stateMonitor: optional adapter, detects StateMonitor usage (plugin, BP nodes, C++ calls)

Each check returns findings in the analyzer's shape plus a data block for the report.
"""
from __future__ import annotations

import html
import json
import re
from collections import defaultdict
from pathlib import Path

SOURCE_EXTENSIONS = (".h", ".hpp", ".cpp", ".inl")
STATE_MONITOR_SYMBOLS = re.compile(
    r"\b(UStateMonitorSubsystem|UStateMonitorLibrary|FStateMonitorModel|SStateMonitor|UStateMonitorWidget)\b")
STATE_MONITOR_SUBMIT = re.compile(r"\b(Submit|SubmitStructSnapshot|SubmitStateSnapshot|SubmitSnapshot)\s*\(")
TEXT_LITERAL = re.compile(r'TEXT\("([^"]+)"\)')


def load_config(paths: list[Path]) -> dict:
    def merge(base: dict, extra: dict) -> dict:
        for k, v in extra.items():
            if isinstance(v, dict) and isinstance(base.get(k), dict):
                merge(base[k], v)
            else:
                base[k] = v
        return base
    cfg: dict = {}
    for p in paths:
        if p and p.exists():
            merge(cfg, json.loads(p.read_text(encoding="utf-8")))
    return cfg


def _class_short(class_path: str) -> str:
    return class_path.rsplit(".", 1)[-1]


def _in_folders(package: str, folders: list[str]) -> bool:
    return any(package == f or package.startswith(f + "/") for f in folders)


# --------------------------------------------------------------------------- naming

def check_naming(ctx: dict, cfg: dict) -> tuple[list, dict]:
    rules = cfg.get("naming", {})
    prefixes: dict = rules.get("prefixes", {})
    ignore = rules.get("ignoreFolders", [])
    violations = []
    for package, class_path in sorted(ctx["asset_class"].items()):
        if _in_folders(package, ignore):
            continue
        cls = _class_short(class_path)
        folder, _, name = package.rpartition("/")
        problems = []
        allowed = prefixes.get(cls)
        if allowed and not any(name.startswith(p) for p in allowed):
            problems.append(f"접두사 {'/'.join(allowed)} 필요")
        if rules.get("forbidAssetsAtContentRoot") and folder in ctx["roots"] and cls != "World":
            problems.append("콘텐츠 루트에 직접 위치")
        if rules.get("forbidSpaces") and " " in name:
            problems.append("이름에 공백")
        if rules.get("forbidNonAsciiNames") and not name.isascii():
            problems.append("이름에 비ASCII 문자")
        if problems:
            violations.append({"asset": package, "class": cls, "problems": problems})
    findings = []
    if violations:
        findings.append({"severity": "warn", "rule": "naming", "message": f"이름·폴더 규칙 위반 에셋 {len(violations)}개",
                         "items": [f"{v['asset']} ({v['class']}): {', '.join(v['problems'])}" for v in violations]})
    return findings, {"violations": violations}


# --------------------------------------------------------------------------- unused / redirectors

def _ini_roots(project_root: Path, roots: list[str]) -> set[str]:
    found = set()
    pattern = re.compile(r"(/(?:%s)/[A-Za-z0-9_/\-]+)" % "|".join(re.escape(r.strip("/")) for r in roots))
    config = project_root / "Config"
    if config.exists():
        for ini in config.rglob("*.ini"):
            for m in pattern.findall(ini.read_text(encoding="utf-8", errors="replace")):
                found.add(m.split(".")[0])
    return found


def check_unused(ctx: dict, cfg: dict) -> tuple[list, dict]:
    rules = cfg.get("unused", {})
    ignore = rules.get("ignoreFolders", [])
    assets = ctx["asset_class"]
    edges = defaultdict(set)
    for src, deps in list(ctx["asset_hard"].items()) + list(ctx["asset_soft"].items()):
        edges[src] |= deps
    roots = {p for p, c in assets.items() if _class_short(c) == "World"}
    ini = _ini_roots(ctx["project_file"].parent, ctx["roots"])
    roots |= {p for p in ini if p in assets}
    roots |= set(rules.get("extraRoots", []))
    seen, stack = set(roots), list(roots)
    while stack:
        for d in edges.get(stack.pop(), ()):
            if d not in seen:
                seen.add(d)
                stack.append(d)
    unused = sorted(p for p in assets if p not in seen and not _in_folders(p, ignore)
                    and _class_short(assets[p]) != "ObjectRedirector")
    redirectors = sorted(p for p, c in assets.items() if _class_short(c) == "ObjectRedirector")
    findings = []
    if redirectors:
        findings.append({"severity": "warn", "rule": "redirector", "message": f"정리되지 않은 리다이렉터 {len(redirectors)}개 (에디터에서 Fix Up 필요)",
                         "items": redirectors})
    if unused:
        findings.append({"severity": "info", "rule": "unused-asset",
                         "message": f"맵·설정에서 도달할 수 없는 에셋 {len(unused)}개 (삭제 후보, 코드에서 경로로 로드하는지 확인 필요)",
                         "items": unused})
    return findings, {"roots": sorted(roots), "iniRoots": sorted(ini), "unused": unused, "redirectors": redirectors}


# --------------------------------------------------------------------------- complexity

def _longest_exec_chain(graph: dict) -> int:
    adj = defaultdict(list)
    for l in graph["links"]:
        if l["kind"] == "exec":
            adj[l["from"]].append(l["to"])
    memo: dict[str, int] = {}

    def depth(n: str, stack: set[str]) -> int:
        if n in memo:
            return memo[n]
        if n in stack:
            return 0  # loop back-edge
        stack.add(n)
        best = 1 + max((depth(c, stack) for c in adj.get(n, ())), default=0)
        stack.discard(n)
        memo[n] = best
        return best

    return max((depth(n["id"], set()) for n in graph["nodes"]), default=0)


def check_complexity(ctx: dict, cfg: dict) -> tuple[list, dict]:
    th = cfg.get("complexity", {})
    rows = []
    for key, bp in sorted(ctx["bp_index"].items()):
        name = bp["asset"].rsplit(".", 1)[-1]
        for graph in bp["graphs"]:
            nodes = len(graph["nodes"])
            branches = sum(1 for n in graph["nodes"]
                           if any(t in n["class"] for t in ("IfThenElse", "Switch", "Select", "MultiGate")))
            chain = _longest_exec_chain(graph)
            over = []
            if nodes > th.get("warnNodesPerGraph", 120):
                over.append("노드 수")
            if branches > th.get("warnBranchesPerGraph", 25):
                over.append("분기 수")
            if chain > th.get("warnExecChainLength", 60):
                over.append("실행 체인 길이")
            rows.append({"blueprint": name, "graph": graph["name"], "kind": graph["kind"], "nodes": nodes,
                         "branches": branches, "execChain": chain, "links": len(graph["links"]), "over": over})
    rows.sort(key=lambda r: (-r["nodes"], r["blueprint"]))
    heavy = [r for r in rows if r["over"]]
    findings = []
    if heavy:
        findings.append({"severity": "warn", "rule": "bp-complexity", "message": f"복잡도 기준을 넘는 BP 그래프 {len(heavy)}개",
                         "items": [f"{r['blueprint']}.{r['graph']}: {', '.join(r['over'])} (노드 {r['nodes']}, 분기 {r['branches']}, 체인 {r['execChain']})" for r in heavy]})
    return findings, {"graphs": rows}


# --------------------------------------------------------------------------- StateMonitor adapter

def check_state_monitor(ctx: dict, cfg: dict) -> tuple[list, dict]:
    sm = cfg.get("stateMonitor", {})
    plugin_name = sm.get("pluginName", "StateMonitor")
    root = ctx["project_file"].parent
    uproject = json.loads(ctx["project_file"].read_text(encoding="utf-8"))
    plugin_file = None
    plugins_dir = root / "Plugins"
    if plugins_dir.exists():
        plugin_file = next((p for p in plugins_dir.rglob(f"{plugin_name}.uplugin")
                            if not any(part.startswith("__Dwo") for part in p.parts)), None)
    listed = next((p for p in uproject.get("Plugins", []) if p.get("Name") == plugin_name), None)
    enabled = bool(listed and listed.get("Enabled", True)) or bool(plugin_file and not listed)

    bp_submits = []
    for key, bp in sorted(ctx["bp_index"].items()):
        for graph in bp["graphs"]:
            for node in graph["nodes"]:
                owner = node.get("memberOwner") or ""
                if node.get("role") == "call" and f"/Script/{plugin_name}." in owner:
                    bp_submits.append({"blueprint": bp["asset"].rsplit(".", 1)[-1], "graph": graph["name"],
                                       "node": node["id"], "function": node.get("member")})

    cpp_refs = []
    source_ids = set()
    source_root = root / "Source"
    if source_root.exists():
        for f in sorted(source_root.rglob("*")):
            if f.suffix not in SOURCE_EXTENSIONS:
                continue
            lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
            uses = any(STATE_MONITOR_SYMBOLS.search(l) for l in lines)
            if not uses:
                continue
            for i, line in enumerate(lines, 1):
                if STATE_MONITOR_SUBMIT.search(line) or STATE_MONITOR_SYMBOLS.search(line):
                    ids = TEXT_LITERAL.findall(line) if STATE_MONITOR_SUBMIT.search(line) else []
                    source_ids.update(ids)
                    cpp_refs.append({"file": str(f.relative_to(root)), "line": i, "code": line.strip()[:160],
                                     "submit": bool(STATE_MONITOR_SUBMIT.search(line)), "sourceIds": ids})

    present = plugin_file is not None
    data = {"present": present, "enabled": enabled and present,
            "plugin": str(plugin_file.relative_to(root)) if plugin_file else None,
            "bpCalls": bp_submits, "cppRefs": cpp_refs, "sourceIds": sorted(source_ids),
            "testFilter": sm.get("testFilter", "StateMonitor.") if present and enabled else None}
    findings = []
    if present:
        submits = sum(1 for r in cpp_refs if r["submit"]) + len(bp_submits)
        findings.append({"severity": "info", "rule": "state-monitor",
                         "message": f"StateMonitor 사용 중: 제출 지점 {submits}곳 (C++ {sum(1 for r in cpp_refs if r['submit'])}, BP {len(bp_submits)}), "
                                    f"Source ID {len(source_ids)}개. 검증 라인에서 '{data['testFilter']}' 테스트를 함께 실행합니다.",
                         "items": [f"{r['file']}:{r['line']}  {r['code']}" for r in cpp_refs if r["submit"]]
                                  + [f"BP {b['blueprint']}.{b['graph']}  {b['function']}" for b in bp_submits]})
    return findings, data


# --------------------------------------------------------------------------- entry points

def run(ctx: dict, cfg: dict) -> tuple[list, dict]:
    findings, data = [], {}
    for name, fn in (("naming", check_naming), ("unused", check_unused),
                     ("complexity", check_complexity), ("stateMonitor", check_state_monitor)):
        f, d = fn(ctx, cfg)
        findings += f
        data[name] = d
    return findings, data


def render_html(data: dict) -> str:
    esc = html.escape
    out = []
    rows = "".join(
        f"<tr><td>{esc(r['blueprint'])}</td><td>{esc(r['graph'])}</td><td>{esc(r['kind'])}</td>"
        f"<td>{r['nodes']}</td><td>{r['branches']}</td><td>{r['execChain']}</td>"
        f"<td>{esc(', '.join(r['over'])) or '-'}</td></tr>" for r in data["complexity"]["graphs"][:30])
    out.append("<section><h2>BP 복잡도</h2><div class='scroll'><table><thead><tr><th>Blueprint</th><th>그래프</th><th>종류</th>"
               f"<th>노드</th><th>분기</th><th>최장 실행 체인</th><th>기준 초과</th></tr></thead><tbody>{rows or '<tr><td colspan=7>없음</td></tr>'}</tbody></table></div></section>")

    nv = data["naming"]["violations"]
    rows = "".join(f"<tr><td><code>{esc(v['asset'])}</code></td><td>{esc(v['class'])}</td><td>{esc(', '.join(v['problems']))}</td></tr>" for v in nv[:200])
    out.append(f"<section><h2>이름 · 폴더 규칙</h2><p class='meta'>위반 {len(nv)}개 · 규칙은 config의 naming 항목</p>"
               f"<div class='scroll'><table><thead><tr><th>에셋</th><th>클래스</th><th>문제</th></tr></thead><tbody>{rows or '<tr><td colspan=3>위반 없음</td></tr>'}</tbody></table></div></section>")

    un = data["unused"]
    out.append("<section><h2>미사용 후보 · 리다이렉터</h2>"
               f"<p class='meta'>도달 시작점: 맵 {sum(1 for r in un['roots'] if r not in un['iniRoots'])}개, 설정 파일 {len(un['iniRoots'])}개</p>"
               f"<h3>도달할 수 없는 에셋 ({len(un['unused'])})</h3><code>{'<br>'.join(esc(u) for u in un['unused'][:200]) or '없음'}</code>"
               f"<h3>리다이렉터 ({len(un['redirectors'])})</h3><code>{'<br>'.join(esc(u) for u in un['redirectors']) or '없음'}</code></section>")

    sm = data["stateMonitor"]
    if sm["present"]:
        refs = "".join(f"<tr><td><code>{esc(r['file'])}:{r['line']}</code></td><td>{'제출' if r['submit'] else '참조'}</td>"
                       f"<td><code>{esc(r['code'])}</code></td></tr>" for r in sm["cppRefs"])
        refs += "".join(f"<tr><td>BP {esc(b['blueprint'])}.{esc(b['graph'])}</td><td>호출</td><td><code>{esc(b['function'] or '')}</code></td></tr>" for b in sm["bpCalls"])
        out.append(f"<section><h2>StateMonitor 연계</h2><p class='meta'>플러그인 {esc(sm['plugin'] or '')} · 활성 {sm['enabled']} · "
                   f"Source ID: {esc(', '.join(sm['sourceIds']) or '없음')} · 검증 필터 <code>{esc(sm['testFilter'] or '-')}</code></p>"
                   f"<div class='scroll'><table><thead><tr><th>위치</th><th>종류</th><th>코드</th></tr></thead><tbody>{refs}</tbody></table></div></section>")
    return "".join(out)

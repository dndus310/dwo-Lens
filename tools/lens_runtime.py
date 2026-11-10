"""dwo-Forge Lens runtime lines: map boot smoke test, CSV performance capture, content sizes, Insights trace.

The engine runs are driven by lens.ps1; this module only enumerates maps, parses what the runs left behind
and writes out/<line>.json plus out/report.html. Standard library only.

    python lens_runtime.py maps   --project <dir> [--out <json>] [--config <json>...]   -> JSON list of map packages
    python lens_runtime.py smoke  --run <run dir> --project <dir> [--config <json>...]  (reads raw/smoke-runs.json)
    python lens_runtime.py perf   --run <run dir> --csv <file> --map <map> [--exit-code n] [--config <json>...]
    python lens_runtime.py sizes  --run <run dir> --project <dir> [--graph <graph.json>] [--config <json>...]
    python lens_runtime.py trace  --run <run dir> --file <utrace> --seconds n [--insights <exe>] [--exit-code n]
"""
from __future__ import annotations

import argparse
import csv
import fnmatch
import html
import json
import math
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lens_logs  # noqa: E402
from lens_analyze import EXTERNAL_PREFIXES, collapse_package, strongly_connected  # noqa: E402
from lens_quality import load_config  # noqa: E402
from lens_verify import ORDER, log_groups_html, page, worst  # noqa: E402

DEFAULTS = {
    "smoke": {"maps": [], "exclude": [], "maxMaps": 20, "seconds": 15, "maxErrors": 0, "maxWarnings": 200},
    "perf": {"map": "", "frames": 600, "warmupFrames": 60, "budgetMs": 33.3, "hitchMs": 100.0,
             "nullrhi": False, "resX": 960, "resY": 540, "timeoutSeconds": 600},
    "sizes": {"warnAssetMB": 100, "top": 50},
    "trace": {"seconds": 20, "channels": "default,counters,loadtime,task", "nullrhi": False},
}
ASSET_EXTS = (".uasset", ".umap", ".uexp", ".ubulk")
STAT_COLUMNS = ("FrameTime", "GameThreadTime", "RenderThreadTime", "RHIThreadTime", "GPUTime")
SERIES_POINTS = 600
CRASH_MARKERS = ("=== Critical error: ===", "Unhandled Exception", "Assertion failed")


def section(cfg: dict, name: str) -> dict:
    return {**DEFAULTS[name], **(cfg.get(name) or {})}


def esc(value) -> str:
    return html.escape(str(value))


def kpis(items: list[tuple]) -> str:
    return "<div class='kpis'>" + "".join(f"<div class='kpi'><b>{esc(v)}</b><span>{esc(k)}</span></div>" for k, v in items) + "</div>"


def mb(n: float) -> str:
    return f"{n / 1048576:.1f} MB"


# =============================================================================== content packages

def package_of(file: Path, content: Path) -> str:
    """Content/Maps/L_Main.umap -> /Game/Maps/L_Main"""
    rel = file.relative_to(content).with_suffix("").as_posix()
    return "/Game/" + rel


def list_maps(project: Path, cfg: dict) -> list[str]:
    s = section(cfg, "smoke")
    if s["maps"]:
        return list(s["maps"])[: int(s["maxMaps"])]
    content = project / "Content"
    maps = []
    for f in sorted(content.rglob("*.umap")) if content.exists() else []:
        pkg = package_of(f, content)
        if any(f"/{p}/" in pkg for p in EXTERNAL_PREFIXES):
            continue
        if any(fnmatch.fnmatch(pkg, g) for g in s["exclude"]):
            continue
        maps.append(pkg)
    return maps[: int(s["maxMaps"])]


# =============================================================================== smoke

def smoke_verdict(m: dict, max_errors: int, max_warnings: int) -> tuple[str, list[str]]:
    """Gate for one map boot. m: errors, warnings, ensures, fatal, crashed, exitCode, killed, logFound."""
    reasons = []
    if not m.get("logFound", True):
        reasons.append("로그 없음 (엔진이 시작되지 않음)")
    if m["fatal"]:
        reasons.append("Fatal")
    if m.get("crashed"):
        reasons.append("크래시")
    if m["ensures"]:
        reasons.append(f"ensure {m['ensures']}")
    if m["errors"] > max_errors:
        reasons.append(f"Error {m['errors']} > {max_errors}")
    if not m.get("killed") and m.get("exitCode") not in (None, 0):
        reasons.append(f"비정상 종료 (exit {m['exitCode']})")
    if reasons:
        return "fail", reasons
    if not m.get("killed") and m.get("exitCode") == 0:
        reasons.append("제한 시간 전에 스스로 종료")
    if m["errors"]:
        reasons.append(f"Error {m['errors']}")
    if m["warnings"] > max_warnings:
        reasons.append(f"Warning {m['warnings']} > {max_warnings}")
    return ("warn" if reasons else "pass"), reasons


def smoke_map(entry: dict, crashes: list[dict], ignore: set[str], s: dict) -> dict:
    log = Path(entry["log"])
    summary = lens_logs.run([log], None, ignore, None)
    ensures = sum(g["count"] for g in summary["groups"] if "Ensure condition failed" in g["sample"] or g["category"] == "Ensure")
    # "LogWindows: Error: === Critical error: ===" parses as an Error; for a boot it means the game died.
    crash_lines = any(any(marker in g["sample"] for marker in CRASH_MARKERS) for g in summary["groups"])
    mine = [c for c in crashes if entry.get("startedAt", 0) - 1 <= c["time"] <= entry.get("endedAt", float("inf")) + 5]
    m = {
        "map": entry["map"], "verdict": None, "exitCode": entry.get("exitCode"),
        "seconds": round(float(entry.get("seconds", 0)), 1), "errors": summary["counts"]["Error"],
        "warnings": summary["counts"]["Warning"], "ensures": ensures,
        "fatal": bool(summary["counts"]["Fatal"]) or crash_lines or bool(mine), "log": str(log),
        "killed": bool(entry.get("killed")), "logFound": log.exists(), "crashed": bool(mine),
        "crashes": mine, "groups": summary["groups"][:20],
    }
    m["verdict"], m["reasons"] = smoke_verdict(m, int(s["maxErrors"]), int(s["maxWarnings"]))
    return m


def smoke(run: Path, project: Path, cfg: dict) -> dict:
    s = section(cfg, "smoke")
    entries = json.loads((run / "raw" / "smoke-runs.json").read_text(encoding="utf-8-sig"))
    if isinstance(entries, dict):
        entries = [entries]
    crashes = lens_logs.parse_crashes(project / "Saved" / "Crashes")
    ignore = set((cfg.get("logs") or {}).get("ignoreCategories", []))
    maps = [smoke_map(e, crashes, ignore, s) for e in entries]
    verdict = worst([m["verdict"] for m in maps]) if maps else "warn"
    return {"schema": "lens.smoke/0.1", "verdict": verdict, "seconds": s["seconds"],
            "maxErrors": s["maxErrors"], "maxWarnings": s["maxWarnings"], "maps": maps,
            "note": "" if maps else "부팅할 맵 없음"}


def render_smoke(result: dict, run_id: str) -> str:
    rows, details = [], []
    for m in result["maps"]:
        rows.append(f"<tr><td><code>{esc(m['map'])}</code></td><td><span class='badge {m['verdict']}'>{m['verdict']}</span></td>"
                    f"<td>{'' if m['exitCode'] is None else esc(m['exitCode'])}{' (종료시킴)' if m['killed'] else ''}</td>"
                    f"<td>{m['seconds']}</td><td>{m['errors']}</td><td>{m['warnings']}</td><td>{m['ensures']}</td>"
                    f"<td>{'yes' if m['fatal'] else ''}</td><td>{esc(', '.join(m['reasons']))}</td></tr>")
        summary = {"groups": m["groups"], "crashes": m["crashes"]}
        details.append(f"<section><h2><code>{esc(m['map'])}</code> <span class='badge {m['verdict']}'>{m['verdict']}</span></h2>"
                       f"<p class='meta'>{esc(m['log'])}</p>{log_groups_html(summary, 20)}</section>")
    counts = {v: sum(1 for m in result["maps"] if m["verdict"] == v) for v in ("pass", "warn", "fail")}
    head = (f"<section><h2>요약</h2>{kpis([('맵', len(result['maps'])), ('pass', counts['pass']), ('warn', counts['warn']), ('fail', counts['fail']), ('맵당 초', result['seconds'])])}"
            f"<p class='meta'>fail: Fatal · 크래시 · ensure · Error &gt; {result['maxErrors']} · 비정상 종료 / warn: Error · Warning &gt; {result['maxWarnings']} · 조기 종료 {esc(result['note'])}</p></section>")
    table = ("<section><h2>맵 부팅</h2><div class='scroll'><table><thead><tr><th>맵</th><th>판정</th><th>exit</th><th>초</th><th>Error</th>"
             f"<th>Warning</th><th>ensure</th><th>Fatal</th><th>사유</th></tr></thead><tbody>{''.join(rows) or '<tr><td colspan=9>없음</td></tr>'}</tbody></table></div></section>")
    return page("맵 부팅 스모크", result["verdict"], run_id, head + table + "".join(details))


# =============================================================================== perf

def read_profile_csv(path: Path) -> dict[str, list[float]]:
    """UE CSV profiler output: header row, one row per frame, then a repeated header and a metadata row."""
    with path.open(encoding="utf-8-sig", errors="replace", newline="") as fh:
        rows = list(csv.reader(fh))
    if not rows:
        return {}
    header = [h.strip() for h in rows[0]]
    columns: dict[str, list[float]] = {h: [] for h in header if h}
    for row in rows[1:]:
        if not row or row[0].startswith("[") or [c.strip() for c in row] == header:
            break
        for name, cell in zip(header, row):
            if name in columns:
                try:
                    columns[name].append(float(cell))
                except ValueError:
                    pass
    return {k: v for k, v in columns.items() if v}


def percentile(sorted_values: list[float], p: float) -> float:
    """Nearest-rank percentile."""
    if not sorted_values:
        return 0.0
    k = max(0, min(len(sorted_values) - 1, math.ceil(p / 100 * len(sorted_values)) - 1))
    return sorted_values[k]


def stats(values: list[float]) -> dict:
    v = sorted(values)
    r = lambda x: round(x, 2)  # noqa: E731
    return {"avg": r(sum(v) / len(v)) if v else 0.0, "p50": r(percentile(v, 50)), "p95": r(percentile(v, 95)),
            "p99": r(percentile(v, 99)), "max": r(v[-1]) if v else 0.0}


def downsample(values: list[float], limit: int = SERIES_POINTS) -> list[float]:
    """Bucket maximum, so hitches survive the reduction."""
    if len(values) <= limit:
        return [round(x, 2) for x in values]
    size = len(values) / limit
    return [round(max(values[int(i * size):max(int((i + 1) * size), int(i * size) + 1)]), 2) for i in range(limit)]


def perf_verdict(frame: dict, hitches: int, budget: float) -> tuple[str, list[str]]:
    if frame["p95"] > budget:
        return "fail", [f"FrameTime p95 {frame['p95']} ms > 예산 {budget} ms"]
    reasons = []
    if frame["p99"] > budget:
        reasons.append(f"p99 {frame['p99']} ms > 예산")
    if hitches:
        reasons.append(f"히치 {hitches}회")
    return ("warn" if reasons else "pass"), reasons


def perf(csv_path: Path | None, map_name: str, cfg: dict, exit_code=None) -> dict:
    p = section(cfg, "perf")
    budget, hitch_ms, warmup = float(p["budgetMs"]), float(p["hitchMs"]), int(p["warmupFrames"])
    base = {"schema": "lens.perf/0.1", "map": map_name, "budgetMs": budget, "hitchMs": hitch_ms,
            "warmupFrames": warmup, "nullrhi": bool(p["nullrhi"]), "exitCode": exit_code,
            "csv": str(csv_path) if csv_path else None}
    columns = read_profile_csv(csv_path) if csv_path and csv_path.exists() else {}
    frames = columns.get("FrameTime", [])[warmup:]
    if not frames:
        return {**base, "verdict": "fail", "reasons": ["CSV 없음 또는 프레임 없음"], "frames": 0, "stats": {}, "hitches": [], "series": {}}
    st = {name: stats(columns[name][warmup:]) for name in STAT_COLUMNS if columns.get(name, [])[warmup:]}
    hitches = [{"frame": i + warmup, "ms": round(ms, 2)} for i, ms in enumerate(frames) if ms > hitch_ms]
    verdict, reasons = perf_verdict(st["FrameTime"], len(hitches), budget)
    series = {name: downsample(columns[name][warmup:]) for name in ("FrameTime", "GameThreadTime", "RenderThreadTime") if name in st}
    return {**base, "verdict": verdict, "reasons": reasons, "frames": len(frames), "stats": st,
            "hitches": hitches[:200], "series": series}


def sparkline(series: list[float], budget: float, width: int = 1000, height: int = 160) -> str:
    if not series:
        return ""
    top = max(max(series), budget) * 1.1
    step = width / max(1, len(series) - 1)
    pts = " ".join(f"{i * step:.1f},{height - v / top * height:.1f}" for i, v in enumerate(series))
    by = height - budget / top * height
    return (f"<svg viewBox='0 0 {width} {height}' preserveAspectRatio='none' style='width:100%;height:{height}px;display:block'>"
            f"<line x1='0' x2='{width}' y1='{by:.1f}' y2='{by:.1f}' stroke='var(--warn)' stroke-dasharray='6 4'/>"
            f"<polyline points='{pts}' fill='none' stroke='var(--det)' stroke-width='1.5' vector-effect='non-scaling-stroke'/></svg>"
            f"<p class='meta'>FrameTime (ms, 구간 최대값 {len(series)}점) · 점선 = 예산 {budget} ms · 최대 {top / 1.1:.1f} ms</p>")


def render_perf(result: dict, run_id: str) -> str:
    st = result["stats"]
    ft = st.get("FrameTime", {})
    p95, budget = ft.get("p95", 0), result["budgetMs"]
    head = (f"<section><h2>요약</h2>{kpis([('맵', result['map']), ('프레임', result['frames']), ('FrameTime p95', f'{p95} ms'), ('예산', f'{budget} ms'), ('히치', len(result['hitches']))])}"
            f"<p class='meta'>{esc(' · '.join(result['reasons']))} · 워밍업 {result['warmupFrames']}프레임 제외 · {'nullrhi' if result['nullrhi'] else '렌더링'} · CSV {esc(result['csv'] or '')}</p></section>")
    rows = "".join(f"<tr><td><code>{esc(n)}</code></td>" + "".join(f"<td>{v[k]}</td>" for k in ("avg", "p50", "p95", "p99", "max")) + "</tr>"
                   for n, v in st.items())
    table = ("<section><h2>스레드별 시간 (ms)</h2><div class='scroll'><table><thead><tr><th>지표</th><th>avg</th><th>p50</th><th>p95</th><th>p99</th><th>max</th></tr></thead>"
             f"<tbody>{rows or '<tr><td colspan=6>데이터 없음</td></tr>'}</tbody></table></div></section>")
    chart = f"<section><h2>프레임 시간</h2>{sparkline(result['series'].get('FrameTime', []), result['budgetMs'])}</section>" if result["series"] else ""
    hitch_rows = "".join(f"<tr><td>{h['frame']}</td><td>{h['ms']}</td></tr>" for h in result["hitches"][:50])
    hitches = (f"<section><h2>히치 (&gt; {result['hitchMs']} ms)</h2><div class='scroll'><table><thead><tr><th>프레임</th><th>ms</th></tr></thead>"
               f"<tbody>{hitch_rows or '<tr><td colspan=2>없음</td></tr>'}</tbody></table></div></section>")
    return page("성능 캡처", result["verdict"], run_id, head + chart + table + hitches)


# =============================================================================== sizes

def scan_sizes(content: Path) -> tuple[dict[str, int], dict[str, list[int]]]:
    """Package -> bytes (all of its files), extension -> [bytes, files]."""
    packages: dict[str, int] = defaultdict(int)
    by_ext: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    if not content.exists():
        return {}, {}
    for f in content.rglob("*"):
        ext = f.suffix.lower()
        if ext not in ASSET_EXTS or not f.is_file():
            continue
        size = f.stat().st_size
        packages[package_of(f, content)] += size
        by_ext[ext][0] += size
        by_ext[ext][1] += 1
    return dict(packages), dict(by_ext)


def folder_of(package: str, depth: int = 2) -> str:
    parts = package.split("/")[1:-1]  # drop leading '' and the asset name
    return "/" + "/".join(parts[: depth + 1]) if parts else "/"


def hard_graph(graph_doc: dict) -> dict[str, set[str]]:
    edges: dict[str, set[str]] = defaultdict(set)
    for r in graph_doc.get("relations", []):
        if r.get("type") == "AssetHardRef":
            edges[r["source"].removeprefix("asset:")].add(r["target"].removeprefix("asset:"))
    return dict(edges)


def closure_weights(graph: dict[str, set[str]], sizes: dict[str, int]) -> dict[str, tuple[int, int]]:
    """Asset -> (bytes of itself plus everything it hard-loads, number of hard-loaded assets excluding itself).
    Cycles are condensed; reach sets are integer bitsets summed byte by byte through lookup tables."""
    nodes = set(graph) | set(sizes)
    for deps in graph.values():
        nodes |= deps
    order = sorted(nodes)
    index = {n: i for i, n in enumerate(order)}
    comp_of, reach = {}, []
    for ci, comp in enumerate(strongly_connected({n: graph.get(n, set()) for n in nodes})):
        mask = 0
        for n in comp:
            comp_of[n] = ci
            mask |= 1 << index[n]
        for n in comp:
            for d in graph.get(n, ()):
                if comp_of[d] != ci:
                    mask |= reach[comp_of[d]]
        reach.append(mask)
    nbytes = (len(order) + 7) // 8
    tables = []
    for b in range(nbytes):
        sz = [sizes.get(order[b * 8 + k], 0) if b * 8 + k < len(order) else 0 for k in range(8)]
        tables.append([sum(sz[k] for k in range(8) if v >> k & 1) for v in range(256)])
    totals: dict[int, tuple[int, int]] = {}
    out = {}
    for n in order:
        ci = comp_of[n]
        if ci not in totals:
            raw = reach[ci].to_bytes(nbytes, "little")
            totals[ci] = (sum(tables[i][v] for i, v in enumerate(raw) if v), bin(reach[ci]).count("1"))
        total, count = totals[ci]
        out[n] = (total, count - 1)
    return out


def sizes(project: Path, cfg: dict, graph_doc: dict | None, graph_path: str | None = None) -> dict:
    s = section(cfg, "sizes")
    top = int(s["top"])
    packages, by_ext = scan_sizes(project / "Content")
    # World Partition external actors/objects are loaded with their map: fold them for closures and folders.
    folded: dict[str, int] = defaultdict(int)
    for pkg, b in packages.items():
        folded[collapse_package(pkg, ["/Game"])] += b
    folders: dict[str, list[int]] = defaultdict(lambda: [0, 0])
    for pkg, b in folded.items():
        f = folders[folder_of(pkg)]
        f[0] += b
        f[1] += 1
    limit = float(s["warnAssetMB"]) * 1048576
    largest = sorted(folded.items(), key=lambda kv: -kv[1])
    over = [{"asset": a, "bytes": b} for a, b in largest if b > limit]
    heaviest = []
    if graph_doc:
        weights = closure_weights(hard_graph(graph_doc), dict(folded))
        heaviest = [{"asset": a, "closureBytes": w[0], "closureCount": w[1]}
                    for a, w in sorted(weights.items(), key=lambda kv: (-kv[1][0], kv[0])) if a in folded and w[1] > 0][:top]
    return {
        "schema": "lens.sizes/0.1", "verdict": "warn" if over else "pass",
        "totalBytes": sum(packages.values()), "assetCount": len(folded), "fileCount": sum(v[1] for v in by_ext.values()),
        "warnAssetMB": s["warnAssetMB"], "overLimit": over,
        "byFolder": [{"folder": k, "bytes": v[0], "count": v[1]} for k, v in sorted(folders.items(), key=lambda kv: -kv[1][0])][:top],
        "byType": [{"ext": k, "bytes": v[0], "count": v[1]} for k, v in sorted(by_ext.items(), key=lambda kv: -kv[1][0])],
        "largest": [{"asset": a, "bytes": b} for a, b in largest[:top]],
        "heaviestClosures": heaviest, "graph": graph_path,
    }


def render_sizes(result: dict, run_id: str) -> str:
    def table(title, cols, rows):
        body = "".join("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>" for r in rows)
        return (f"<section><h2>{title}</h2><div class='scroll'><table><thead><tr>{''.join(f'<th>{c}</th>' for c in cols)}</tr></thead>"
                f"<tbody>{body or f'<tr><td colspan={len(cols)}>없음</td></tr>'}</tbody></table></div></section>")
    limit = result["warnAssetMB"]
    head = (f"<section><h2>요약</h2>{kpis([('전체', mb(result['totalBytes'])), ('에셋', result['assetCount']), ('파일', result['fileCount']), (f'{limit} MB 초과', len(result['overLimit']))])}"
            f"<p class='meta'>World Partition 외부 액터는 맵에 합산 · 하드 참조 가중치: {esc(result['graph'] or '분석(analyze) 결과 없음')}</p></section>")
    flag = lambda b: f" <span class='badge warn'>&gt; {limit} MB</span>" if b > limit * 1048576 else ""  # noqa: E731
    return page("콘텐츠 용량", result["verdict"], run_id, head
                + table("큰 에셋", ["에셋", "크기"], [(f"<code>{esc(x['asset'])}</code>{flag(x['bytes'])}", mb(x["bytes"])) for x in result["largest"]])
                + table("하드 참조 로드 무게 (자신 + 하드 참조로 함께 로드되는 에셋)", ["에셋", "합계", "함께 로드"],
                        [(f"<code>{esc(x['asset'])}</code>", mb(x["closureBytes"]), x["closureCount"]) for x in result["heaviestClosures"]])
                + table("폴더", ["폴더", "크기", "에셋"], [(f"<code>{esc(x['folder'])}</code>", mb(x["bytes"]), x["count"]) for x in result["byFolder"]])
                + table("파일 형식", ["확장자", "크기", "파일"], [(esc(x["ext"]), mb(x["bytes"]), x["count"]) for x in result["byType"]]))


# =============================================================================== trace

def trace(file: Path, seconds: float, insights: str | None, exit_code=None, killed: bool = True) -> dict:
    size = file.stat().st_size if file.exists() else 0
    reasons = []
    if not size:
        reasons.append("trace 파일 없음")
    if not killed and exit_code not in (None, 0):
        reasons.append(f"비정상 종료 (exit {exit_code})")
    verdict = "fail" if reasons else "pass"
    return {"schema": "lens.trace/0.1", "verdict": verdict, "reasons": reasons, "file": str(file), "bytes": size,
            "seconds": round(float(seconds), 1), "exitCode": exit_code, "insights": insights}


def render_trace(result: dict, run_id: str, channels: str) -> str:
    open_cmd = f"\"{result['insights']}\" -OpenTraceFile=\"{result['file']}\"" if result["insights"] else "UnrealInsights.exe 없음"
    body = (f"<section><h2>요약</h2>{kpis([('크기', mb(result['bytes'])), ('캡처 초', result['seconds']), ('채널', channels)])}"
            f"<p class='meta'>{esc(' · '.join(result['reasons']))}</p></section>"
            f"<section><h2>Unreal Insights로 열기</h2><code>{esc(open_cmd)}</code></section>")
    return page("Insights 트레이스", result["verdict"], run_id, body)


# =============================================================================== main

def write_outputs(run: Path, name: str, result: dict, report: str) -> None:
    out = run / "out"
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{name}.json").write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
    (out / "report.html").write_text(report, encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("maps", "smoke", "perf", "sizes", "trace"):
        p = sub.add_parser(name)
        p.add_argument("--config", nargs="*", type=Path, default=[])
        if name != "maps":
            p.add_argument("--run", type=Path, required=True)
        if name in ("maps", "smoke", "sizes"):
            p.add_argument("--project", type=Path, required=True)
        if name == "maps":
            p.add_argument("--out", type=Path)
        if name in ("perf", "trace"):
            p.add_argument("--exit-code", type=int)
        if name == "perf":
            p.add_argument("--csv", type=Path)
            p.add_argument("--map", default="")
        if name == "sizes":
            p.add_argument("--graph", type=Path)
        if name == "trace":
            p.add_argument("--file", type=Path, required=True)
            p.add_argument("--seconds", type=float, default=0)
            p.add_argument("--insights")
            p.add_argument("--exited", action="store_true", help="the game exited on its own (not stopped)")
    args = ap.parse_args()
    cfg = load_config(args.config or [Path(__file__).resolve().parent.parent / "config" / "default.json"])

    if args.cmd == "maps":
        text = json.dumps(list_maps(args.project, cfg), ensure_ascii=False)
        if args.out:
            args.out.write_text(text, encoding="utf-8")
        else:
            print(text)
        return 0
    run_id = args.run.name
    if args.cmd == "smoke":
        result = smoke(args.run, args.project, cfg)
        write_outputs(args.run, "smoke", result, render_smoke(result, run_id))
        print("[dwo-forge] smoke: " + ", ".join(f"{m['map']}={m['verdict']}" for m in result["maps"]) + f" -> {result['verdict']}")
    elif args.cmd == "perf":
        result = perf(args.csv, args.map, cfg, args.exit_code)
        write_outputs(args.run, "perf", result, render_perf(result, run_id))
        ft = result["stats"].get("FrameTime", {})
        print(f"[dwo-forge] perf: frames={result['frames']} FrameTime avg={ft.get('avg')} p95={ft.get('p95')} "
              f"hitches={len(result['hitches'])} -> {result['verdict']}")
    elif args.cmd == "sizes":
        graph_doc = json.loads(args.graph.read_text(encoding="utf-8")) if args.graph and args.graph.exists() else None
        result = sizes(args.project, cfg, graph_doc, str(args.graph) if graph_doc else None)
        write_outputs(args.run, "sizes", result, render_sizes(result, run_id))
        print(f"[dwo-forge] sizes: {result['assetCount']} assets, {mb(result['totalBytes'])}, "
              f"over limit {len(result['overLimit'])}, closures {len(result['heaviestClosures'])} -> {result['verdict']}")
    else:
        result = trace(args.file, args.seconds, args.insights, args.exit_code, killed=not args.exited)
        write_outputs(args.run, "trace", result, render_trace(result, run_id, section(cfg, "trace")["channels"]))
        print(f"[dwo-forge] trace: {mb(result['bytes'])} in {result['seconds']} s -> {result['verdict']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

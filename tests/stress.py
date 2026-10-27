"""dwo-Assay stress tests: large synthetic inputs for the analyzer, log parser and quality checks.

    python tests/stress.py [--scale 1.0] [--out .qa/stress]

Prints a JSON summary (sizes, seconds, peak Python memory) and exits non-zero on any failure.
"""
from __future__ import annotations

import argparse
import json
import random
import shutil
import sys
import time
import tracemalloc
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import assay_analyze as analyze  # noqa: E402
import assay_logs as logs  # noqa: E402
import assay_quality as quality  # noqa: E402


def measure(fn, *args):
    tracemalloc.start()
    t0 = time.perf_counter()
    value = fn(*args)
    seconds = time.perf_counter() - t0
    _, peak = tracemalloc.get_traced_memory()
    tracemalloc.stop()
    return value, round(seconds, 2), round(peak / 1024 / 1024, 1)


# --------------------------------------------------------------------------- synthetic project

def make_raw(root: Path, n_native: int, n_bp: int, n_assets: int, nodes_per_graph: int, seed: int = 7) -> Path:
    rnd = random.Random(seed)
    raw = root / "raw"
    raw.mkdir(parents=True)
    (root / "Stress.uproject").write_text(json.dumps({"Modules": [{"Name": "Stress"}]}), encoding="utf-8")
    (root / "Source" / "Stress").mkdir(parents=True)
    (root / "Source" / "Stress" / "Stress.Build.cs").write_text(
        'PublicDependencyModuleNames.AddRange(new string[] { "Core", "Engine", "UMG" });', encoding="utf-8")

    native = []
    for i in range(n_native):
        parent = f"/Script/Stress.C{i - 1}" if i % 50 else "/Script/Engine.Actor"  # chains of 50
        props = [{"name": f"P{k}", "cppType": "UObject*", "replicated": k == 0, "blueprintVisible": True,
                  "refs": [{"kind": "hard" if k % 2 else "soft", "target": f"/Script/Stress.C{rnd.randrange(n_native)}"}]}
                 for k in range(4)]
        native.append({"path": f"/Script/Stress.C{i}", "name": f"C{i}", "package": "/Script/Stress", "origin": "native",
                       "super": parent, "abstract": False, "interface": False, "interfaces": [],
                       "functions": [{"name": f"F{k}", "flags": ["BlueprintCallable"], "overrides": "", "paramRefs": []} for k in range(5)],
                       "properties": props})

    blueprints, bp_classes = [], []
    for i in range(n_bp):
        cls = f"/Game/BP/BP_{i}.BP_{i}_C"
        bp_classes.append({"path": cls, "name": f"BP_{i}_C", "package": f"/Game/BP/BP_{i}", "origin": "blueprint",
                           "super": f"/Script/Stress.C{rnd.randrange(n_native)}", "abstract": False, "interface": False,
                           "interfaces": [], "functions": [], "properties": []})
        graphs = []
        for gi in range(2):
            nodes = [{"id": f"{i:06d}{gi}{k:05d}", "class": "K2Node_Event" if k == 0 else
                      ("K2Node_IfThenElse" if k % 9 == 0 else "K2Node_CallFunction"), "title": f"Node {k}",
                      "role": "event" if k == 0 else "call", "member": f"F{k % 5}",
                      "memberOwner": f"/Script/Stress.C{rnd.randrange(n_native)}"} for k in range(nodes_per_graph)]
            links = [{"from": nodes[k]["id"], "fromPin": "then", "to": nodes[k + 1]["id"], "toPin": "execute", "kind": "exec"}
                     for k in range(nodes_per_graph - 1)]
            links.append({"from": nodes[-1]["id"], "fromPin": "then", "to": nodes[1]["id"], "toPin": "execute", "kind": "exec"})
            graphs.append({"name": f"G{gi}", "path": f"{cls}:G{gi}", "kind": "event", "nodes": nodes, "links": links})
        blueprints.append({"asset": f"/Game/BP/BP_{i}.BP_{i}", "package": f"/Game/BP/BP_{i}", "assetClass": "Blueprint",
                           "blueprintType": "BPTYPE_Normal", "parent": bp_classes[-1]["super"], "generatedClass": cls,
                           "interfaces": [], "variables": [], "graphs": graphs})

    assets = []
    for i in range(n_assets):
        pkg = f"/Game/BP/BP_{i}" if i < n_bp else f"/Game/Data/A_{i}"
        hard = [f"/Game/Data/A_{i + 1}"] if i + 1 < n_assets and i >= n_bp else []  # one very long chain
        hard += [f"/Game/Data/A_{rnd.randrange(n_bp, n_assets)}" for _ in range(3)]
        if i % 1000 == 0 and i + 2 < n_assets:
            hard.append(f"/Game/Data/A_{max(n_bp, i - 2)}")  # some cycles
        assets.append({"package": pkg, "asset": f"{pkg}.X", "class": "/Script/Engine.Blueprint" if i < n_bp else "/Script/Engine.DataAsset",
                       "hard": hard + ["/Script/Engine"], "soft": [f"/Game/Data/A_{rnd.randrange(n_bp, n_assets)}"]})
    assets.append({"package": "/Game/Maps/Main", "asset": "/Game/Maps/Main.Main", "class": "/Script/Engine.World",
                   "hard": [f"/Game/BP/BP_{k}" for k in range(0, n_bp, 3)] + [f"/Game/Data/A_{n_bp}"], "soft": []})

    meta = {"schema": "assay.raw/0.1", "engineVersion": "5.7.4-stress", "project": str(root / "Stress.uproject"),
            "extractedAtUtc": "2026-10-28T00:00:00Z", "projectModules": ["Stress"], "contentRoots": ["/Game"],
            "blueprintLoadFailures": 0}
    for name, doc in (("meta", meta), ("reflection", {"native": native, "blueprint": bp_classes}),
                      ("blueprints", {"blueprints": blueprints}), ("assets", {"assets": assets})):
        (raw / f"{name}.json").write_text(json.dumps(doc), encoding="utf-8")
    return raw


def analyzer_stress(work: Path, scale: float) -> dict:
    n_native, n_bp, n_assets, nodes = int(3000 * scale), int(1500 * scale), int(30000 * scale), 80
    raw = make_raw(work / "project", n_native, n_bp, n_assets, nodes)
    raw_mb = sum(f.stat().st_size for f in raw.iterdir()) / 1024 / 1024

    def run():
        out = work / "out"
        sys.argv = ["assay_analyze.py", "--raw", str(raw), "--project", str(work / "project" / "Stress.uproject"),
                    "--out", str(out), "--run-id", "stress", "--config", str(ROOT / "config" / "default.json")]
        analyze.main()
        return json.loads((out / "findings.json").read_text(encoding="utf-8"))

    findings, seconds, peak = measure(run)
    rules = sorted({f["rule"] for f in findings["findings"]})
    ok = "asset-hard-cycle" in rules and (work / "out" / "report.html").exists()
    return {"name": "analyzer", "ok": ok, "seconds": seconds, "peakMB": peak, "rawMB": round(raw_mb, 1),
            "inputs": {"nativeClasses": n_native, "blueprints": n_bp, "graphNodes": n_bp * 2 * nodes, "assets": n_assets},
            "rules": rules}


def log_stress(work: Path, scale: float) -> dict:
    lines = int(1_000_000 * scale)
    path = work / "big.log"
    rnd = random.Random(3)
    cats = ["LogTemp", "LogBlueprint", "LogStreaming", "LogNet", "LogAI"]
    with path.open("w", encoding="utf-8") as f:
        for i in range(lines):
            r = rnd.random()
            sev = "Error: " if r < 0.01 else ("Warning: " if r < 0.06 else "")
            f.write(f"[2026.10.28-00.00.00:{i % 1000:03d}][{i % 999:3d}]{cats[i % 5]}: {sev}Message {rnd.randrange(200)} at C:\\x\\{i}.uasset\n")
    size_mb = path.stat().st_size / 1024 / 1024
    summary, seconds, peak = measure(lambda: logs.run([path], None, set(), None))
    ok = summary["counts"]["Error"] > 0 and len(summary["groups"]) <= 5 * 200 * 2
    return {"name": "log-parser", "ok": ok, "seconds": seconds, "peakMB": peak, "lines": lines, "fileMB": round(size_mb, 1),
            "linesPerSecond": int(lines / max(seconds, 0.001)), "groups": len(summary["groups"]), "counts": summary["counts"]}


def chain_stress() -> dict:
    n = 200_000
    graph = {str(i): {str(i + 1)} for i in range(n)}
    graph[str(n)] = {"0"}  # one giant cycle
    cycles, seconds, peak = measure(analyze.tarjan, graph)
    nodes = [{"id": str(i), "class": "K2Node_CallFunction"} for i in range(20_000)]
    links = [{"from": str(i), "to": str(i + 1), "kind": "exec"} for i in range(19_999)]
    chain, s2, _ = measure(quality._longest_exec_chain, {"nodes": nodes, "links": links})
    ok = len(cycles) == 1 and len(cycles[0]) == n + 1 and chain == 20_000
    return {"name": "deep-graphs", "ok": ok, "seconds": round(seconds + s2, 2), "peakMB": peak,
            "tarjanNodes": n + 1, "execChainNodes": chain}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--out", type=Path, default=ROOT / ".qa" / "stress")
    args = ap.parse_args()
    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    results = [analyzer_stress(args.out / "analyzer", args.scale), log_stress(args.out, args.scale), chain_stress()]
    report = {"scale": args.scale, "ok": all(r["ok"] for r in results), "results": results}
    (args.out / "stress-result.json").write_text(json.dumps(report, indent=1), encoding="utf-8")
    print(json.dumps(report, indent=1))
    return 0 if report["ok"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

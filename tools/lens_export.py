"""CI export of a finished Lens run: out/junit.xml (JUnit) and out/results.sarif (SARIF 2.1.0).

Jenkins, GitHub Actions, GitLab and TeamCity read JUnit for pass/fail trends; SARIF carries every problem with a
stable fingerprint and baselineState (new / unchanged against the previous run of the same check), so a quality
gate can fail only on new problems.

    python lens_export.py --run <run dir> [--baseline <previous results.sarif>] [--copy-to <dir>]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path
from xml.sax.saxutils import escape, quoteattr

SARIF_SCHEMA = "https://json.schemastore.org/sarif-2.1.0.json"
CHECK_NAMES = {
    "architecture-report": "아키텍처 분석", "verify": "검증", "logs": "로그 분석", "smoke": "맵 스모크",
    "perf": "성능 캡처", "sizes": "콘텐츠 용량", "trace": "Insights 트레이스", "compile": "C++ 컴파일", "package": "패키지 빌드",
}
LEVEL = {"fail": "error", "error": "error", "fatal": "error", "warn": "warning", "warning": "warning", "info": "note"}


def load(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8-sig")) if path.exists() else None
    except (OSError, ValueError):
        return None


def fingerprint(*parts) -> str:
    return hashlib.sha256("\x1f".join(str(p) for p in parts).encode("utf-8")).hexdigest()[:32]


# ----------------------------------------------------------------------------------------------- problems

def problems(run: Path, line: str) -> list[dict]:
    """Every problem of the run as {rule, level, message, location, file, line, new?}."""
    out = run / "out"
    found: list[dict] = []

    def add(rule, level, message, location="", file="", line_no=0, new=None):
        found.append({"rule": rule, "level": LEVEL.get(str(level).lower(), "warning"), "message": str(message),
                      "location": str(location), "file": str(file), "line": int(line_no or 0), "new": new})

    findings = load(out / "findings.json") or {}
    for f in findings.get("findings", []):
        items = f.get("items") or [""]
        for item in items[:500]:
            add(f.get("rule", "finding"), f.get("severity", "warn"), f.get("message", ""), item)
    logs = load(out / "logs.json") or {}
    for g in logs.get("groups", []):
        add(f"log/{g.get('category', 'Log')}", g.get("severity", "Warning"), g.get("sample", ""),
            (g.get("where") or [""])[0], new=g.get("new"))
    for c in logs.get("crashes", []):
        add("crash", "error", c.get("error") or "crash", c.get("folder", ""), new=c.get("new"))
    verify = load(out / "verify.json") or {}
    for step in verify.get("steps", []):
        for t in (step.get("automation") or {}).get("tests", []):
            if t.get("state") not in ("Success", None):
                add("automation-test", "error", f"{t.get('name')}: {t.get('state')}", t.get("name", ""))
    smoke = load(out / "smoke.json") or {}
    for m in smoke.get("maps", []):
        if m.get("verdict") in ("fail", "warn"):
            add("smoke", m["verdict"], " · ".join(m.get("reasons") or []) or "map smoke", m.get("map", ""))
    for name in ("compile", "package"):
        b = load(out / f"{name}.json") or {}
        for e in b.get("errors", []):
            add(f"{name}/{e.get('code') or 'error'}", "error", e.get("message", ""), "", e.get("file", ""), e.get("line", 0))
    for name in ("perf", "sizes"):
        r = load(out / f"{name}.json") or {}
        for reason in r.get("reasons", []) if r.get("verdict") in ("fail", "warn") else []:
            add(name, r["verdict"], reason)
        for a in r.get("overLimit", []) if name == "sizes" else []:
            add("sizes/over-limit", "warn", f"{a.get('asset')} {a.get('bytes', 0) / 1048576:.1f} MB", a.get("asset", ""))
    for p in found:
        p["fingerprint"] = fingerprint(p["rule"], p["message"], p["location"], p["file"])
    return found


# ----------------------------------------------------------------------------------------------- SARIF

def sarif(run: Path, line: str, verdict: str, baseline: dict | None) -> dict:
    known = None
    if baseline:
        known = {r.get("partialFingerprints", {}).get("dwoForge/v1")
                 for rn in baseline.get("runs", []) for r in rn.get("results", [])}
    results, rules = [], {}
    for p in problems(run, line):
        rules.setdefault(p["rule"], {"id": p["rule"], "name": p["rule"]})
        r = {"ruleId": p["rule"], "level": p["level"], "message": {"text": p["message"] or p["rule"]},
             "partialFingerprints": {"dwoForge/v1": p["fingerprint"]}}
        loc = {}
        if p["file"]:
            phys = {"artifactLocation": {"uri": Path(p["file"]).as_uri() if Path(p["file"]).is_absolute() else p["file"]}}
            if p["line"]:
                phys["region"] = {"startLine": p["line"]}
            loc["physicalLocation"] = phys
        if p["location"]:
            loc["logicalLocations"] = [{"fullyQualifiedName": p["location"]}]
        if loc:
            r["locations"] = [loc]
        if p["new"] is not None:
            r["baselineState"] = "new" if p["new"] else "unchanged"
        elif known is not None:
            r["baselineState"] = "unchanged" if p["fingerprint"] in known else "new"
        results.append(r)
    new = sum(1 for r in results if r.get("baselineState") == "new")
    return {
        "$schema": SARIF_SCHEMA, "version": "2.1.0",
        "runs": [{
            "tool": {"driver": {"name": "dwo-Forge Lens", "rules": sorted(rules.values(), key=lambda r: r["id"])}},
            "automationDetails": {"id": f"{line}/{run.name}"},
            "properties": {"check": line, "verdict": verdict, "newResults": new, "baselineCompared": baseline is not None},
            "results": results,
        }],
    }


# ----------------------------------------------------------------------------------------------- JUnit

def junit(run: Path, line: str, run_doc: dict) -> str:
    name = CHECK_NAMES.get(line, line)
    cases: list[tuple[str, str, str, float]] = []   # (name, verdict, detail, seconds)
    for s in run_doc.get("stations", []):
        cases.append((f"{s.get('id', '')} {s.get('name', '')}".strip(), s.get("verdict") or "pass",
                      f"exit code {s.get('exitCode')}" if s.get("exitCode") not in (None, 0) else "", (s.get("ms") or 0) / 1000.0))
    verify = load(run / "out" / "verify.json") or {}
    for step in verify.get("steps", []):
        for t in (step.get("automation") or {}).get("tests", []):
            ok = t.get("state") == "Success"
            cases.append((f"automation {t.get('name')}", "pass" if ok else "fail", t.get("state", ""), float(t.get("duration") or 0)))
    findings = load(run / "out" / "findings.json") or {}
    for f in findings.get("findings", []):
        cases.append((f"rule {f.get('rule')}", f.get("severity", "warn"), f.get("message", ""), 0.0))
    for kind in ("compile", "package"):
        for e in (load(run / "out" / f"{kind}.json") or {}).get("errors", [])[:200]:
            where = f"{e.get('file')}({e.get('line')})" if e.get("file") else ""
            cases.append((f"{kind} {e.get('code')} {where}".strip(), "fail", e.get("message", ""), 0.0))
    if not cases:
        cases.append((name, run_doc.get("final", "pass"), "", 0.0))
    failures = sum(1 for c in cases if c[1] == "fail")
    total = sum(c[3] for c in cases)
    body = []
    for case, verdict, detail, secs in cases:
        attrs = f"classname={quoteattr('dwo-forge.' + line)} name={quoteattr(case)} time=\"{secs:.3f}\""
        if verdict == "fail":
            body.append(f"    <testcase {attrs}><failure message={quoteattr(detail or 'fail')}>{escape(detail)}</failure></testcase>")
        elif verdict == "warn":
            body.append(f"    <testcase {attrs}><system-out>{escape('warn: ' + detail)}</system-out></testcase>")
        else:
            body.append(f"    <testcase {attrs}/>")
    return ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            f"<testsuites name=\"dwo-Forge\" tests=\"{len(cases)}\" failures=\"{failures}\" time=\"{total:.3f}\">\n"
            f"  <testsuite name={quoteattr(name)} id={quoteattr(run.name)} tests=\"{len(cases)}\" failures=\"{failures}\" "
            f"errors=\"0\" skipped=\"0\" time=\"{total:.3f}\">\n" + "\n".join(body) + "\n  </testsuite>\n</testsuites>\n")


def export(run: Path, baseline_path: Path | None, copy_to: Path | None) -> dict:
    run_doc = load(run / "run.json") or {}
    line = run_doc.get("line") or run.name.split("-")[0]
    verdict = run_doc.get("final", "fail")
    out = run / "out"
    out.mkdir(parents=True, exist_ok=True)
    baseline = load(baseline_path) if baseline_path else None
    doc = sarif(run, line, verdict, baseline)
    (out / "results.sarif").write_text(json.dumps(doc, ensure_ascii=False, indent=1), encoding="utf-8")
    (out / "junit.xml").write_text(junit(run, line, run_doc), encoding="utf-8")
    summary = {"run": run.name, "line": line, "verdict": verdict, "results": len(doc["runs"][0]["results"]),
               "new": doc["runs"][0]["properties"]["newResults"]}
    if copy_to:
        dest = copy_to / run.name
        dest.mkdir(parents=True, exist_ok=True)
        for name in ("junit.xml", "results.sarif", "report.html"):
            if (out / name).exists():
                shutil.copy2(out / name, dest / name)
        (dest / "summary.json").write_text(json.dumps(summary, ensure_ascii=False), encoding="utf-8")
    return summary


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", type=Path, required=True)
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--copy-to", type=Path)
    args = ap.parse_args()
    s = export(args.run, args.baseline, args.copy_to)
    print(f"[dwo-forge] export: junit.xml, results.sarif ({s['results']} results, {s['new']} new)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

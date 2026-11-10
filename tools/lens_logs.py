"""dwo-Forge Lens structured log parser (ST-4).

Turns Unreal log files and crash folders into grouped, fingerprinted events so every wrapped tool
(commandlets, automation, editor sessions) reports in one shape. Standard library only.

    python lens_logs.py --files a.log b.log --out logs.json [--crashes <Saved/Crashes>] [--baseline prev.json]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import xml.etree.ElementTree as ET
from collections import OrderedDict
from pathlib import Path

SCHEMA = "lens.logs/0.1"
SEVERITIES = ("Fatal", "Error", "Warning")

LINE_TS = re.compile(
    r"^\[(?P<ts>[\d.\-:]+)\]\[\s*(?P<frame>\d+)\](?P<cat>[A-Za-z][A-Za-z0-9_]*): "
    r"(?:(?P<sev>Fatal|Error|Warning|Display|Log|Verbose|VeryVerbose): )?(?P<msg>.*)$")
LINE_PLAIN = re.compile(
    r"^(?P<cat>Log[A-Za-z0-9_]*|[A-Z][A-Za-z0-9]*): (?P<sev>Fatal|Error|Warning): (?P<msg>.*)$")
# Lines that are failures even without an explicit severity.
SPECIAL = [
    (re.compile(r"Ensure condition failed"), "Error", "Ensure"),
    (re.compile(r"Assertion failed"), "Fatal", "Assert"),
    (re.compile(r"=== Critical error: ==="), "Fatal", "Critical"),
    (re.compile(r"Unhandled Exception"), "Fatal", "Crash"),
]

_NORMALIZERS = [
    (re.compile(r"0x[0-9A-Fa-f]+"), "<addr>"),
    (re.compile(r"\b[0-9A-Fa-f]{32}\b"), "<guid>"),
    (re.compile(r"\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b"), "<guid>"),
    (re.compile(r"[A-Za-z]:[\\/][^\s'\"]+"), "<path>"),
    (re.compile(r"\b\d+(\.\d+)?\b"), "<n>"),
]


def fingerprint(category: str, message: str) -> str:
    text = message
    for pattern, token in _NORMALIZERS:
        text = pattern.sub(token, text)
    return hashlib.sha1(f"{category}|{text}".encode("utf-8")).hexdigest()[:12]


def parse_file(path: Path, ignore: set[str]) -> list[dict]:
    events = []
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        m = LINE_TS.match(line) or LINE_PLAIN.match(line)
        sev = cat = msg = None
        if m and m.group("sev") in SEVERITIES:
            sev, cat, msg = m.group("sev"), m.group("cat"), m.group("msg")
        else:
            for pattern, special_sev, special_cat in SPECIAL:
                if pattern.search(line):
                    sev, cat, msg = special_sev, (m.group("cat") if m else special_cat), (m.group("msg") if m else line.strip())
                    break
        if sev and cat not in ignore:
            detail = []
            if sev == "Fatal":
                # Capture the call stack that follows a fatal line.
                j = i + 1
                while j < len(lines) and len(detail) < 40 and lines[j].strip():
                    detail.append(lines[j].strip())
                    j += 1
            events.append({"file": path.name, "line": i + 1, "severity": sev, "category": cat,
                           "message": msg.strip(), "detail": detail, "fingerprint": fingerprint(cat, msg)})
        i += 1
    return events


def parse_crashes(folder: Path) -> list[dict]:
    crashes = []
    if not folder or not folder.exists():
        return crashes
    for ctx in sorted(folder.glob("*/CrashContext.runtime-xml")):
        entry = {"folder": ctx.parent.name, "error": "", "callstack": [], "time": ctx.stat().st_mtime}
        try:
            root = ET.parse(ctx).getroot()
            def find(tag):
                node = root.find(f".//{tag}")
                return node.text.strip() if node is not None and node.text else ""
            entry["error"] = find("ErrorMessage")
            entry["callstack"] = [l.strip() for l in find("CallStack").splitlines() if l.strip()][:40]
            entry["engineVersion"] = find("EngineVersion")
        except ET.ParseError as exc:
            entry["error"] = f"unreadable CrashContext: {exc}"
        crashes.append(entry)
    return crashes


def summarize(events: list[dict], crashes: list[dict], baseline: dict | None = None) -> dict:
    groups: "OrderedDict[str, dict]" = OrderedDict()
    for e in events:
        g = groups.setdefault(e["fingerprint"], {
            "fingerprint": e["fingerprint"], "severity": e["severity"], "category": e["category"],
            "sample": e["message"], "count": 0, "where": [], "detail": e["detail"],
        })
        g["count"] += 1
        if len(g["where"]) < 5:
            g["where"].append(f"{e['file']}:{e['line']}")
    order = {"Fatal": 0, "Error": 1, "Warning": 2}
    ordered = sorted(groups.values(), key=lambda g: (order[g["severity"]], -g["count"]))
    known = {g["fingerprint"] for g in (baseline or {}).get("groups", [])}
    for g in ordered:
        g["new"] = bool(baseline) and g["fingerprint"] not in known
    counts = {s: sum(1 for e in events if e["severity"] == s) for s in SEVERITIES}
    return {"schema": SCHEMA, "counts": counts, "crashes": crashes, "groups": ordered,
            "newGroups": sum(1 for g in ordered if g["new"])}


def verdict_of(summary: dict, warn_on_new: bool = True) -> str:
    if summary["counts"]["Fatal"] or summary["crashes"]:
        return "fail"
    if summary["counts"]["Error"]:
        return "fail" if (warn_on_new and summary["newGroups"]) else "warn"
    if summary["counts"]["Warning"]:
        return "warn"
    return "pass"


def run(files: list[Path], crashes_dir: Path | None, ignore: set[str], baseline: dict | None) -> dict:
    events = []
    for f in files:
        if f.exists():
            events += parse_file(f, ignore)
    summary = summarize(events, parse_crashes(crashes_dir) if crashes_dir else [], baseline)
    summary["files"] = [str(f) for f in files]
    return summary


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--files", nargs="+", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--crashes", type=Path)
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--ignore", nargs="*", default=[])
    args = ap.parse_args()
    baseline = json.loads(args.baseline.read_text(encoding="utf-8")) if args.baseline and args.baseline.exists() else None
    summary = run(args.files, args.crashes, set(args.ignore), baseline)
    summary["verdict"] = verdict_of(summary)
    args.out.write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
    print(f"[dwo-forge] logs: {summary['counts']} groups={len(summary['groups'])} new={summary['newGroups']} "
          f"crashes={len(summary['crashes'])} verdict={summary['verdict']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

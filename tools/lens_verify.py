"""dwo-Forge Lens verify (ST-3) and logs (ST-4) lines: turn wrapped tool outputs into one verdict and report.

    python lens_verify.py verify --run <run dir> [--baseline <previous logs.json>]
    python lens_verify.py logs   --run <run dir> --files <log files...> [--crashes <dir>] [--baseline <json>]

verify expects in <run dir>: compile.log, datavalidation.log, automation.log, automation/index.json (any may be missing).
"""
from __future__ import annotations

import argparse
import html
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lens_logs  # noqa: E402
from lens_analyze import REPORT_CSS  # noqa: E402

ORDER = {"fail": 0, "warn": 1, "info": 2, "pass": 3}


def worst(verdicts: list[str]) -> str:
    present = [v for v in verdicts if v in ORDER and v != "info"]
    return min(present, key=lambda v: ORDER[v]) if present else "pass"


def parse_automation(folder: Path) -> dict | None:
    index = folder / "index.json"
    if not index.exists():
        return None
    data = json.loads(index.read_text(encoding="utf-8-sig"))
    tests = []
    for t in data.get("tests", []):
        errors = [e["event"]["message"] for e in t.get("entries", []) if e.get("event", {}).get("type") == "Error"]
        warnings = [e["event"]["message"] for e in t.get("entries", []) if e.get("event", {}).get("type") == "Warning"]
        tests.append({"name": t.get("fullTestPath") or t.get("testDisplayName"), "state": t.get("state"),
                      "errors": errors[:10], "warnings": warnings[:10]})
    summary = {k: data.get(k, 0) for k in ("succeeded", "succeededWithWarnings", "failed", "notRun")}
    return {"summary": summary, "tests": tests}


def verify(run: Path, baseline: dict | None) -> dict:
    steps = []
    for step_id, name, log_name in (("V1", "Blueprint 전체 컴파일", "compile.log"),
                                    ("V2", "에셋 데이터 검증", "datavalidation.log")):
        log = run / log_name
        if not log.exists():
            steps.append({"id": step_id, "name": name, "verdict": "skip", "summary": None})
            continue
        s = lens_logs.run([log], None, set(), baseline)
        steps.append({"id": step_id, "name": name, "verdict": lens_logs.verdict_of(s, warn_on_new=False), "summary": s})

    automation = parse_automation(run / "automation")
    if automation is None:
        verdict = "skip" if not (run / "automation.log").exists() else "fail"
        steps.append({"id": "V3", "name": "자동화 테스트", "verdict": verdict, "automation": None,
                      "note": "index.json 없음" if verdict == "fail" else "실행 안 함"})
    else:
        s = automation["summary"]
        total = sum(s.values())
        verdict = "fail" if s["failed"] else ("warn" if s["succeededWithWarnings"] else ("info" if total == 0 else "pass"))
        steps.append({"id": "V3", "name": "자동화 테스트", "verdict": verdict, "automation": automation,
                      "note": "필터와 일치하는 테스트 없음" if total == 0 else ""})
    final = worst([s["verdict"] for s in steps if s["verdict"] != "skip"])
    return {"schema": "lens.verify/0.1", "verdict": final, "steps": steps}


# --------------------------------------------------------------------------- html

def log_groups_html(summary: dict, limit: int = 40) -> str:
    esc = html.escape
    rows = []
    for g in summary["groups"][:limit]:
        sev = {"Fatal": "fail", "Error": "fail", "Warning": "warn"}[g["severity"]]
        new = " <span class='badge info'>new</span>" if g.get("new") else ""
        detail = f"<details><summary>스택</summary><code>{'<br>'.join(esc(d) for d in g['detail'])}</code></details>" if g["detail"] else ""
        rows.append(f"<tr><td><span class='badge {sev}'>{g['severity']}</span>{new}</td><td>{g['count']}</td>"
                    f"<td><code>{esc(g['category'])}</code></td><td>{esc(g['sample'][:300])}{detail}"
                    f"<div class='meta'>{esc(', '.join(g['where']))}</div></td></tr>")
    crashes = "".join(f"<tr><td><span class='badge fail'>crash</span></td><td>1</td><td><code>{esc(c['folder'])}</code></td>"
                      f"<td>{esc(c['error'])}<details><summary>콜스택</summary><code>{'<br>'.join(esc(l) for l in c['callstack'])}</code></details></td></tr>"
                      for c in summary.get("crashes", []))
    body = crashes + "".join(rows)
    return ("<div class='scroll'><table><thead><tr><th>심각도</th><th>횟수</th><th>카테고리</th><th>내용</th></tr></thead>"
            f"<tbody>{body or '<tr><td colspan=4>오류·경고 없음</td></tr>'}</tbody></table></div>")


def page(title: str, verdict: str, run_id: str, body: str) -> str:
    esc = html.escape
    return (f"<!doctype html><html lang='ko'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
            f"<title>{esc(title)}</title><style>{REPORT_CSS}</style></head><body><main>"
            f"<section><div class='meta'>{esc(run_id)}</div><h1>{esc(title)} <span class='badge {verdict}'>{verdict}</span></h1></section>"
            f"{body}</main></body></html>")


def render_verify(result: dict, run_id: str) -> str:
    esc = html.escape
    parts = []
    for step in result["steps"]:
        head = f"<section><h2>{esc(step['id'])} {esc(step['name'])} <span class='badge {step['verdict'] if step['verdict'] != 'skip' else 'info'}'>{step['verdict']}</span></h2>"
        if step.get("summary"):
            c = step["summary"]["counts"]
            parts.append(head + f"<p class='meta'>Error {c['Error']} · Warning {c['Warning']} · Fatal {c['Fatal']}</p>"
                         + log_groups_html(step["summary"]) + "</section>")
        elif step.get("automation"):
            a = step["automation"]
            s = a["summary"]
            rows = "".join(
                f"<tr><td><code>{esc(t['name'] or '')}</code></td><td><span class='badge {'pass' if t['state'] == 'Success' else 'fail'}'>{esc(t['state'] or '')}</span></td>"
                f"<td>{'<br>'.join(esc(e) for e in t['errors'])}</td></tr>" for t in a["tests"])
            parts.append(head + f"<p class='meta'>성공 {s['succeeded']} · 경고 포함 성공 {s['succeededWithWarnings']} · 실패 {s['failed']} · 미실행 {s['notRun']} {esc(step.get('note', ''))}</p>"
                         f"<div class='scroll'><table><thead><tr><th>테스트</th><th>결과</th><th>오류</th></tr></thead><tbody>{rows or '<tr><td colspan=3>없음</td></tr>'}</tbody></table></div></section>")
        else:
            parts.append(head + f"<p class='meta'>{esc(step.get('note', '실행 안 함'))}</p></section>")
    return page("검증 라인", result["verdict"], run_id, "".join(parts))


# --------------------------------------------------------------------------- main

def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("verify")
    v.add_argument("--run", type=Path, required=True)
    v.add_argument("--baseline", type=Path)
    lg = sub.add_parser("logs")
    lg.add_argument("--run", type=Path, required=True)
    lg.add_argument("--files", nargs="*", type=Path, default=[])
    lg.add_argument("--crashes", type=Path)
    lg.add_argument("--baseline", type=Path)
    lg.add_argument("--ignore", nargs="*", default=[])
    args = ap.parse_args()

    baseline = None
    if args.baseline and args.baseline.exists():
        baseline = json.loads(args.baseline.read_text(encoding="utf-8"))
    run_id = args.run.name
    out = args.run / "out"
    out.mkdir(parents=True, exist_ok=True)

    if args.cmd == "verify":
        result = verify(args.run, baseline)
        (out / "verify.json").write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
        (out / "report.html").write_text(render_verify(result, run_id), encoding="utf-8")
        print(f"[dwo-forge] verify: " + ", ".join(f"{s['id']}={s['verdict']}" for s in result["steps"]) + f" -> {result['verdict']}")
    else:
        summary = lens_logs.run(args.files, args.crashes, set(args.ignore), baseline)
        summary["verdict"] = lens_logs.verdict_of(summary)
        (out / "logs.json").write_text(json.dumps(summary, ensure_ascii=False, indent=1), encoding="utf-8")
        c = summary["counts"]
        body = (f"<section><h2>요약</h2><p class='meta'>파일 {len(summary['files'])}개 · Fatal {c['Fatal']} · Error {c['Error']} · "
                f"Warning {c['Warning']} · 그룹 {len(summary['groups'])} · 새 그룹 {summary['newGroups']} · 크래시 {len(summary['crashes'])}</p>"
                f"<code>{'<br>'.join(html.escape(f) for f in summary['files'])}</code></section>"
                f"<section><h2>오류 · 경고 그룹</h2>{log_groups_html(summary, 80)}</section>")
        (out / "report.html").write_text(page("로그 라인", summary["verdict"], run_id, body), encoding="utf-8")
        print(f"[dwo-forge] logs: {c} groups={len(summary['groups'])} new={summary['newGroups']} "
              f"crashes={len(summary['crashes'])} verdict={summary['verdict']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

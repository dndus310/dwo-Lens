"""Lens build lines: editor C++ compile (UnrealBuildTool) and package build (RunUAT BuildCookRun).

forge.ps1 runs the tools and keeps their console output in the run folder; this script turns that output into
out/<line>.json and out/report.html like the other lines.

    python lens_build.py compile --run <run dir> [--skipped "<reason>"]
    python lens_build.py package --run <run dir> [--archive <dir>]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lens_runtime import esc, kpis, mb  # noqa: E402
from lens_verify import page  # noqa: E402

SCHEMA = "lens.build/0.1"
# MSVC / clang style: path(line): error C1234: message   |   path(line,col): warning ...
DIAG = re.compile(r"^(?P<file>[A-Za-z]:[^()\n]+?)\((?P<line>\d+)(?:,\d+)?\)\s*:\s*(?P<kind>fatal error|error|warning)\s+(?P<code>[A-Z]+\d+)?:?\s*(?P<msg>.*)$")
LINKER = re.compile(r"^(?P<file>[^:\n]+?)\s*:\s*(?P<kind>fatal error|error|warning)\s+(?P<code>LNK\d+)\s*:\s*(?P<msg>.*)$")
UBT_ERROR = re.compile(r"^(?:ERROR|Error)\s*:\s*(?P<msg>.+)$")
UAT_ERROR = re.compile(r"(?:Log\w+:\s*)?Error:\s*(?P<msg>.+)$")
UAT_WARNING = re.compile(r"(?:Log\w+:\s*)?Warning:\s*(?P<msg>.+)$")
LIVE_CODING = "Live Coding"
SKIP_REASONS = {"blueprint-only": "Blueprint 전용 프로젝트라 컴파일할 C++ 코드가 없습니다."}


def read_lines(path: Path) -> list[str]:
    if not path.exists():
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def parse_compile(lines: list[str]) -> dict:
    errors, warnings, seen = [], 0, set()
    for raw in lines:
        line = raw.strip()
        m = DIAG.match(line) or LINKER.match(line)
        if m:
            kind = m.group("kind")
            if kind == "warning":
                warnings += 1
                continue
            key = (m.group("file"), m.groupdict().get("line"), m.group("msg"))
            if key in seen:
                continue
            seen.add(key)
            errors.append({"file": m.group("file").strip(), "line": int(m.groupdict().get("line") or 0),
                           "code": m.group("code") or "", "message": m.group("msg").strip()})
            continue
        m = UBT_ERROR.match(line)
        if m and m.group("msg") not in seen:
            seen.add(m.group("msg"))
            errors.append({"file": "", "line": 0, "code": "UBT", "message": m.group("msg").strip()})
    return {"errors": errors, "warnings": warnings}


def compile_line(run: Path, skipped: str | None) -> dict:
    raw = run / "raw"
    meta = json.loads((raw / "compile-meta.json").read_text(encoding="utf-8-sig")) if (raw / "compile-meta.json").exists() else {}
    if skipped:
        skipped = SKIP_REASONS.get(skipped, skipped)
        return {"schema": SCHEMA, "line": "compile", "verdict": "pass", "skipped": skipped, "errors": [], "warnings": 0, **meta}
    lines = read_lines(raw / "compile.log")
    parsed = parse_compile(lines)
    code = meta.get("exitCode")
    live_coding = any(LIVE_CODING in l and "active" in l.lower() for l in lines)
    if code != 0 or parsed["errors"]:
        verdict = "fail"
    elif parsed["warnings"]:
        verdict = "warn"
    else:
        verdict = "pass"
    hint = "에디터의 Live Coding이 켜져 있으면 빌드할 수 없습니다. 에디터를 닫고 다시 실행하세요." if live_coding else ""
    return {"schema": SCHEMA, "line": "compile", "verdict": verdict, **meta, **parsed, "hint": hint,
            "tail": lines[-40:] if verdict == "fail" else []}


def folder_bytes(path: Path) -> int:
    if not path.exists():
        return 0
    return sum(f.stat().st_size for f in path.rglob("*") if f.is_file())


def package_line(run: Path, archive: Path | None) -> dict:
    raw = run / "raw"
    meta = json.loads((raw / "package-meta.json").read_text(encoding="utf-8-sig")) if (raw / "package-meta.json").exists() else {}
    lines = read_lines(raw / "package.log")
    compile_part = parse_compile(lines)
    errors = list(compile_part["errors"])
    warnings = compile_part["warnings"]
    seen = {e["message"] for e in errors}
    for line in lines:
        m = UAT_ERROR.search(line)
        if m and "Error: 0" not in line and m.group("msg").strip() not in seen:
            seen.add(m.group("msg").strip())
            errors.append({"file": "", "line": 0, "code": "UAT", "message": m.group("msg").strip()})
        elif UAT_WARNING.search(line):
            warnings += 1
    success = any("BUILD SUCCESSFUL" in l for l in lines)
    code = meta.get("exitCode")
    size = folder_bytes(archive) if archive else 0
    exes = sorted(str(p.relative_to(archive)) for p in archive.rglob("*.exe")) if archive and archive.exists() else []
    if code != 0 or not success:
        verdict = "fail"
    elif errors:
        verdict = "warn"   # UAT reported errors (cook) but still produced a build
    else:
        verdict = "pass"
    return {"schema": SCHEMA, "line": "package", "verdict": verdict, **meta, "success": success,
            "archive": str(archive) if archive else "", "archiveBytes": size, "executables": exes,
            "errors": errors[:200], "errorCount": len(errors), "warnings": warnings,
            "tail": lines[-60:] if verdict == "fail" else []}


def errors_table(errors: list[dict]) -> str:
    if not errors:
        return "<p class='meta'>오류 없음</p>"
    rows = "".join(f"<tr><td><code>{esc(e['code'])}</code></td><td><code>{esc(e['file'])}{':' + str(e['line']) if e['line'] else ''}</code></td>"
                   f"<td>{esc(e['message'])}</td></tr>" for e in errors[:200])
    return f"<div class='scroll'><table><thead><tr><th>코드</th><th>위치</th><th>내용</th></tr></thead><tbody>{rows}</tbody></table></div>"


def render(result: dict, run_id: str) -> str:
    title = "C++ 컴파일" if result["line"] == "compile" else "패키지 빌드"
    parts = []
    if result.get("skipped"):
        parts.append(f"<section><p>{esc(result['skipped'])}</p></section>")
    else:
        stats = [("오류", len(result.get("errors", [])) if result["line"] == "compile" else result.get("errorCount", 0)),
                 ("경고", result.get("warnings", 0)), ("종료 코드", result.get("exitCode", "-")),
                 ("소요", f"{result.get('seconds', 0):.0f}초")]
        if result["line"] == "package":
            stats.append(("결과물", mb(result.get("archiveBytes", 0))))
        parts.append(f"<section>{kpis(stats)}")
        meta = " · ".join(esc(f"{k} {result[k]}") for k in ("target", "platform", "configuration") if result.get(k))
        if meta:
            parts.append(f"<p class='meta'>{meta}</p>")
        if result.get("hint"):
            parts.append(f"<p><b>{esc(result['hint'])}</b></p>")
        parts.append("</section>")
        if result["line"] == "package" and result.get("archive"):
            exe = "".join(f"<li><code>{esc(e)}</code></li>" for e in result.get("executables", []))
            parts.append(f"<section><h2>결과물</h2><p><code>{esc(result['archive'])}</code></p><ul>{exe}</ul></section>")
        parts.append(f"<section><h2>오류</h2>{errors_table(result.get('errors', []))}</section>")
        if result.get("tail"):
            parts.append("<section><h2>마지막 출력</h2><pre>" + esc("\n".join(result["tail"])) + "</pre></section>")
    return page(title, result["verdict"], run_id, "".join(parts))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("line", choices=["compile", "package"])
    ap.add_argument("--run", type=Path, required=True)
    ap.add_argument("--skipped")
    ap.add_argument("--archive", type=Path)
    args = ap.parse_args()
    result = compile_line(args.run, args.skipped) if args.line == "compile" else package_line(args.run, args.archive)
    out = args.run / "out"
    out.mkdir(parents=True, exist_ok=True)
    (out / f"{args.line}.json").write_text(json.dumps(result, ensure_ascii=False, indent=1), encoding="utf-8")
    (out / "report.html").write_text(render(result, args.run.name), encoding="utf-8")
    count = len(result.get("errors", [])) if args.line == "compile" else result.get("errorCount", 0)
    print(f"[dwo-forge] {args.line}: errors={count} warnings={result.get('warnings', 0)} verdict={result['verdict']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

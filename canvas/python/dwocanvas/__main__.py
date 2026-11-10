"""DwoCanvas CLI.

    python -m dwocanvas validate <file> [--json] [--schema-dir DIR]
    python -m dwocanvas migrate <in> <out> [--schema-dir DIR]
    python -m dwocanvas naming <in> <out> [--rules FILE] [--schema-dir DIR]

Exit codes: 0 = no errors, 1 = the (resulting) document has errors, 2 = usage or IO error.
<out> may be "-" for stdout (the report then goes to stderr).
"""

from __future__ import annotations

import argparse
import json
import sys

from . import migrate as migrate_module
from . import naming
from .schema import SCHEMA_V2
from .validate import LEVELS, count_levels, has_errors, validate_document

EXIT_OK, EXIT_ERRORS, EXIT_USAGE = 0, 1, 2


class UsageError(Exception):
    pass


class _Parser(argparse.ArgumentParser):
    def error(self, message):  # argparse exits with 2 already; keep the message format
        self.print_usage(sys.stderr)
        print(f"dwocanvas: error: {message}", file=sys.stderr)
        sys.exit(EXIT_USAGE)


def _configure_stdio() -> None:
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def read_json(path: str):
    try:
        if path == "-":
            return json.load(sys.stdin)
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except OSError as e:
        raise UsageError(f"cannot read {path}: {e.strerror or e}") from e
    except json.JSONDecodeError as e:
        raise UsageError(f"{path} is not valid JSON: {e}") from e


def write_json(path: str, doc) -> None:
    text = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    try:
        if path == "-":
            sys.stdout.write(text)
            return
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
    except OSError as e:
        raise UsageError(f"cannot write {path}: {e.strerror or e}") from e


def format_issue(issue: dict) -> str:
    where = ""
    if issue.get("node") or issue.get("name"):
        where = f"{issue.get('node', '')} '{issue.get('name', '')}': "
    return f"  [{issue.get('rule', '-')}] {where}{issue.get('message', '')}"


def format_report(label: str, issues: list[dict], source_schema=None) -> str:
    """Human report: header, issues grouped by level (error, warning, info), summary line."""
    lines = [label + (f" ({source_schema} -> {SCHEMA_V2})" if source_schema and source_schema != SCHEMA_V2 else f" ({SCHEMA_V2})")]
    counts = count_levels(issues)
    for level in LEVELS:
        group = [i for i in issues if i.get("level") == level]
        if group:
            lines.append(f"{level.upper()} ({len(group)})")
            lines.extend(format_issue(i) for i in group)
    lines.append(f"Summary: {counts['error']} error(s), {counts['warning']} warning(s), {counts['info']} info")
    return "\n".join(lines)


def cmd_validate(args) -> int:
    doc = read_json(args.file)
    issues, _ = validate_document(doc, args.schema_dir)
    if args.json:
        sys.stdout.write(json.dumps(issues, indent=1, ensure_ascii=False) + "\n")
    else:
        print(format_report(args.file, issues, doc.get("schema") if isinstance(doc, dict) else None))
    return EXIT_ERRORS if has_errors(issues) else EXIT_OK


def _write_and_report(args, doc, label_schema) -> int:
    issues, _ = validate_document(doc, args.schema_dir)
    write_json(args.output, doc)
    report = format_report(args.output, issues, label_schema)
    print(report, file=sys.stderr if args.output == "-" else sys.stdout)
    return EXIT_ERRORS if has_errors(issues) else EXIT_OK


def cmd_migrate(args) -> int:
    doc = read_json(args.input)
    if not isinstance(doc, dict):
        raise UsageError(f"{args.input}: document must be a JSON object")
    return _write_and_report(args, migrate_module.migrate(doc), None)


def cmd_naming(args) -> int:
    doc = read_json(args.input)
    if not isinstance(doc, dict):
        raise UsageError(f"{args.input}: document must be a JSON object")
    try:
        rules = naming.load_rules(args.rules) if args.rules else naming.default_rules()
    except OSError as e:
        raise UsageError(f"cannot read {args.rules}: {e.strerror or e}") from e
    except (ValueError, json.JSONDecodeError) as e:
        raise UsageError(f"bad naming rules {args.rules}: {e}") from e
    doc = naming.apply_naming(migrate_module.migrate(doc), rules)
    return _write_and_report(args, doc, None)


def build_parser() -> argparse.ArgumentParser:
    parser = _Parser(prog="python -m dwocanvas", description="DwoCanvas design document tools (dwocanvas/2).")
    sub = parser.add_subparsers(dest="command", parser_class=_Parser)

    p = sub.add_parser("validate", help="schema + rule check (v1 input is migrated first)")
    p.add_argument("file", help="design JSON ('-' = stdin)")
    p.add_argument("--json", action="store_true", help="print the issues as a JSON array of lint items")
    p.add_argument("--schema-dir", help="directory with dwocanvas.schema.json (default: plugin/DwoCanvas/Schema)")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("migrate", help="designbridge/1 -> dwocanvas/2 (v2 input is copied unchanged)")
    p.add_argument("input")
    p.add_argument("output", help="output file ('-' = stdout)")
    p.add_argument("--schema-dir")
    p.set_defaults(func=cmd_migrate)

    p = sub.add_parser("naming", help="fill role / variable / widgetName from naming rules (migrates v1 first)")
    p.add_argument("input")
    p.add_argument("output", help="output file ('-' = stdout)")
    p.add_argument("--rules", help="naming rules JSON (default: dwocanvas/naming.json)")
    p.add_argument("--schema-dir")
    p.set_defaults(func=cmd_naming)
    return parser


def main(argv=None) -> int:
    _configure_stdio()
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as e:  # --help (0) or usage error (2)
        return e.code if isinstance(e.code, int) else EXIT_USAGE
    if not getattr(args, "func", None):
        parser.print_usage(sys.stderr)
        return EXIT_USAGE
    try:
        return args.func(args)
    except UsageError as e:
        print(f"dwocanvas: error: {e}", file=sys.stderr)
        return EXIT_USAGE
    except OSError as e:  # e.g. schema file missing
        print(f"dwocanvas: error: {e}", file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())

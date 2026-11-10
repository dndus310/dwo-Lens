"""Lightweight JavaScript sanity checker (no Node needed).

Not a parser: it tokenizes (comments, strings, template literals, regex literals) and reports
- unbalanced / mismatched brackets, unterminated strings / comments / templates / regexes
- Python-isms used as identifiers (True, False, None, elif, def, lambda, and, or)
- doubled commas
- duplicate top-level function / const / let declarations
- ``await`` outside an async function

    python canvas/tools/check_js.py file.js [file.html ...]     (HTML: inline <script> blocks are checked)

Exit code 0 = clean, 1 = findings, 2 = IO error.
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass

PUNCT_BEFORE_REGEX = set("(,=:[!&|?{};+-*%<>~^")
KEYWORDS_BEFORE_REGEX = {"return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "throw", "case", "do", "else", "yield", "await"}
NOT_FUNCTION_CALLS = {"if", "for", "while", "switch", "catch", "with", "return", "typeof", "await", "function"}
PYTHONISMS = {"True", "False", "None", "elif", "def", "lambda", "and", "or"}
OPEN = {"(": ")", "[": "]", "{": "}"}
CLOSE = {")": "(", "]": "[", "}": "{"}
MULTI_PUNCT = ["===", "!==", "**=", "...", "&&=", "||=", "??=", ">>>", "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.",
               "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "**"]


@dataclass
class Token:
    kind: str  # ident, num, str, tmpl, regex, punct
    value: str
    line: int


class JsError(Exception):
    def __init__(self, line: int, message: str):
        super().__init__(f"line {line}: {message}")
        self.line = line


def tokenize(src: str, findings: list[str], line_offset: int = 0) -> list[Token]:
    tokens: list[Token] = []
    i, n, line = 0, len(src), 1 + line_offset
    brackets: list[tuple[str, int]] = []  # "(" "[" "{" or "${" (template expression)

    def prev_allows_regex() -> bool:
        if not tokens:
            return True
        t = tokens[-1]
        if t.kind == "punct":
            return t.value[-1] in PUNCT_BEFORE_REGEX and t.value not in (")", "]", "++", "--")
        return t.kind == "ident" and t.value in KEYWORDS_BEFORE_REGEX

    def scan_template(start: int, start_line: int) -> tuple[int, int, bool]:
        """Scan template text from ``start`` (after ` or }). Returns (index, line, opened_expression)."""
        j, ln = start, start_line
        while j < n:
            c = src[j]
            if c == "\\":
                j += 2
                continue
            if c == "\n":
                ln += 1
            if c == "`":
                return j + 1, ln, False
            if c == "$" and j + 1 < n and src[j + 1] == "{":
                return j + 2, ln, True
            j += 1
        raise JsError(start_line, "unterminated template literal")

    try:
        while i < n:
            c = src[i]
            if c == "\n":
                line += 1
                i += 1
            elif c in " \t\r\f\v﻿":
                i += 1
            elif src.startswith("//", i):
                j = src.find("\n", i)
                i = n if j < 0 else j
            elif src.startswith("/*", i):
                j = src.find("*/", i + 2)
                if j < 0:
                    raise JsError(line, "unterminated /* comment")
                line += src.count("\n", i, j)
                i = j + 2
            elif c in "'\"":
                j, start_line = i + 1, line
                while True:
                    if j >= n or src[j] == "\n":
                        raise JsError(start_line, "unterminated string")
                    if src[j] == "\\":
                        j += 2
                        continue
                    if src[j] == c:
                        break
                    j += 1
                tokens.append(Token("str", src[i:j + 1], line))
                i = j + 1
            elif c == "`":
                start_line = line
                j, line, opened = scan_template(i + 1, line)
                tokens.append(Token("tmpl", "`", start_line))
                if opened:
                    brackets.append(("${", line))
                i = j
            elif c == "}" and brackets and brackets[-1][0] == "${":
                brackets.pop()
                j, line, opened = scan_template(i + 1, line)
                if opened:
                    brackets.append(("${", line))
                tokens.append(Token("tmpl", "`", line))
                i = j
            elif c == "/" and prev_allows_regex():
                j, in_class, start_line = i + 1, False, line
                while True:
                    if j >= n or src[j] == "\n":
                        raise JsError(start_line, "unterminated regex literal")
                    if src[j] == "\\":
                        j += 2
                        continue
                    if src[j] == "[":
                        in_class = True
                    elif src[j] == "]":
                        in_class = False
                    elif src[j] == "/" and not in_class:
                        break
                    j += 1
                j += 1
                while j < n and (src[j].isalnum() or src[j] == "_"):
                    j += 1
                tokens.append(Token("regex", src[i:j], line))
                i = j
            elif c.isalpha() or c in "_$":
                m = re.compile(r"[A-Za-z0-9_$]+").match(src, i)
                tokens.append(Token("ident", m.group(0), line))
                i = m.end()
            elif c.isdigit() or (c == "." and i + 1 < n and src[i + 1].isdigit()):
                m = re.compile(r"(0[xXbBoO][0-9a-fA-F_]+|[0-9_]*\.?[0-9_]+([eE][+-]?[0-9]+)?)n?").match(src, i)
                tokens.append(Token("num", m.group(0), line))
                i = m.end()
            else:
                punct = next((p for p in MULTI_PUNCT if src.startswith(p, i)), c)
                if punct in OPEN:
                    brackets.append((punct, line))
                elif punct in CLOSE:
                    if not brackets:
                        raise JsError(line, f"unexpected '{punct}'")
                    opener, opened_line = brackets.pop()
                    if opener != CLOSE[punct]:
                        raise JsError(line, f"'{punct}' closes '{opener}' opened on line {opened_line}")
                tokens.append(Token("punct", punct, line))
                i += len(punct)
        if brackets:
            opener, opened_line = brackets[-1]
            raise JsError(opened_line, f"'{opener}' is never closed")
    except JsError as e:
        findings.append(str(e))
    return tokens


def _matching_open(tokens: list[Token], close_index: int) -> int:
    depth = 0
    for k in range(close_index, -1, -1):
        v = tokens[k].value if tokens[k].kind == "punct" else ""
        if v in CLOSE:
            depth += 1
        elif v in OPEN:
            depth -= 1
            if depth == 0:
                return k
    return -1


def lint_tokens(tokens: list[Token], findings: list[str]) -> None:
    depth = 0
    top_level: dict[str, int] = {}
    # function scopes: (is_async, kind, bracket depth); kind "block" ends at its "}", "expr" ends at , ; or a closer
    scopes: list[tuple[bool, str, int]] = []
    pending_block: bool | None = None  # next "{" opens a function body (value = async)

    for k, t in enumerate(tokens):
        prev = tokens[k - 1] if k else None
        nxt = tokens[k + 1] if k + 1 < len(tokens) else None

        if t.kind == "ident" and (prev is None or prev.value not in (".", "?.")):
            if t.value in PYTHONISMS and not (nxt and nxt.value == ":" and prev and prev.value in ("{", ",")):
                findings.append(f"line {t.line}: '{t.value}' looks like Python")
            if t.value == "await" and not (scopes and scopes[-1][0]):
                findings.append(f"line {t.line}: 'await' outside an async function")
            if depth == 0 and t.value in ("function", "const", "let") and nxt and nxt.kind == "ident":
                name = nxt.value
                if name in top_level:
                    findings.append(f"line {nxt.line}: '{name}' is declared again (first on line {top_level[name]})")
                else:
                    top_level[name] = nxt.line

        if t.kind != "punct":
            continue
        v = t.value
        if v == "," and nxt and nxt.value == ",":
            findings.append(f"line {t.line}: doubled comma")

        # end of expression-bodied arrows
        while scopes and scopes[-1][1] == "expr" and ((v in (",", ";") and depth == scopes[-1][2]) or (v in CLOSE and depth <= scopes[-1][2])):
            scopes.pop()

        if v == "=>":
            # async (a) => / async a =>
            is_async = False
            if prev and prev.value == ")":
                o = _matching_open(tokens, k - 1)
                is_async = o > 0 and tokens[o - 1].value == "async"
            elif prev and prev.kind == "ident":
                is_async = k >= 2 and tokens[k - 2].value == "async"
            if nxt and nxt.value == "{":
                pending_block = is_async
            else:
                scopes.append((is_async, "expr", depth))
        elif v == "{":
            if pending_block is not None:
                scopes.append((pending_block, "block", depth + 1))
                pending_block = None
            elif prev and prev.value == ")":
                o = _matching_open(tokens, k - 1)
                before = tokens[o - 1] if o > 0 else None
                if before is not None and before.kind == "ident" and before.value not in NOT_FUNCTION_CALLS:
                    # function name(...) {  /  async name(...) {  /  method(...) {
                    lead = tokens[o - 2] if o > 1 else None
                    is_async = lead is not None and lead.value == "async" or (
                        lead is not None and lead.value == "function" and o > 2 and tokens[o - 3].value == "async")
                    scopes.append((is_async, "block", depth + 1))
                elif before is not None and before.value == "function":
                    scopes.append((o > 1 and tokens[o - 2].value == "async", "block", depth + 1))
            depth += 1
        elif v in ("(", "["):
            depth += 1
        elif v in CLOSE:
            if v == "}" and scopes and scopes[-1][1] == "block" and scopes[-1][2] == depth:
                scopes.pop()
            depth -= 1


def check_source(src: str, line_offset: int = 0) -> list[str]:
    findings: list[str] = []
    tokens = tokenize(src, findings, line_offset)
    if not findings:
        lint_tokens(tokens, findings)
    return findings


def check_html(html: str) -> list[str]:
    findings: list[str] = []
    for m in re.finditer(r"<script\b[^>]*>(.*?)</script>", html, re.S | re.I):
        findings += check_source(m.group(1), html.count("\n", 0, m.start(1)))
    return findings


def check_file(path: str) -> list[str]:
    with open(path, encoding="utf-8") as f:
        text = f.read()
    return check_html(text) if path.lower().endswith((".html", ".htm")) else check_source(text)


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__.strip().splitlines()[0], file=sys.stderr)
        print("usage: check_js.py file.js [file.html ...]", file=sys.stderr)
        return 2
    status = 0
    for path in argv:
        try:
            findings = check_file(path)
        except OSError as e:
            print(f"{path}: {e}", file=sys.stderr)
            return 2
        for finding in findings:
            print(f"{path}: {finding}")
        status = status or (1 if findings else 0)
        if not findings:
            print(f"{path}: ok")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

"""Figma adapter checks without Node: naming constant parity, tables shared with Python, JS sanity."""

import json
import re
import sys
import unittest

import support
from dwocanvas import migrate, naming

sys.path.insert(0, str(support.CANVAS_DIR / "tools"))
import check_js  # noqa: E402

CODE_JS = support.FIGMA_DIR / "code.js"
UI_HTML = support.FIGMA_DIR / "ui.html"
MOCKUP_JS = support.FIGMA_DIR / "samples" / "DwoMockup" / "code.js"


def code():
    return CODE_JS.read_text(encoding="utf-8")


class NamingParity(unittest.TestCase):
    def test_naming_constant_matches_naming_json(self):
        m = re.search(r"// <naming\.json>[^\n]*\nconst NAMING = (\{.*?\});\n// </naming\.json>", code(), re.S)
        self.assertIsNotNone(m, "NAMING block markers not found in code.js")
        self.assertEqual(json.loads(m.group(1)), json.loads(naming.DEFAULT_RULES_PATH.read_text(encoding="utf-8")))

    def test_ue_role_table_matches_migration(self):
        m = re.search(r"const UE_ROLES = \{(.*?)\};", code(), re.S)
        pairs = dict(re.findall(r"(\w+): \"(\w+)\"", m.group(1)))
        self.assertEqual(pairs, migrate.UE_ROLE_MAP)

    def test_font_weight_table_matches_migration(self):
        m = re.search(r"const FONT_WEIGHTS = (\[.*?\n\]);", code(), re.S)
        table = json.loads(re.sub(r",(\s*[\]}])", r"\1", m.group(1)))
        self.assertEqual([(tuple(words), weight) for words, weight in table], migrate._WEIGHTS)

    def test_extract_capabilities_match(self):
        m = re.search(r"const EXTRACT_CAPABILITIES = (\[.*?\]);", code())
        self.assertEqual(json.loads(m.group(1)), migrate.EXTRACT_CAPABILITIES)

    def test_emits_v2(self):
        src = code()
        self.assertIn('const SCHEMA = "dwocanvas/2";', src)
        self.assertIn('const ADAPTER = "dwocanvas-figma/0.2.0";', src)
        self.assertIn('tool: "figma", adapter: ADAPTER, mode: "extract"', src)
        self.assertIn('".dwocanvas.json"', src)
        self.assertNotIn("designbridge", src)
        self.assertNotIn("images", src)
        self.assertNotIn("p.token", src)

    def test_user_facing_names(self):
        manifest = json.loads((support.FIGMA_DIR / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["name"], "DwoCanvas Export")
        builder = json.loads((support.FIGMA_DIR / "samples" / "DwoMockup" / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(builder["name"], "DwoCanvas - DWO mockup builder")
        self.assertIn("<h1>DwoCanvas Export</h1>", UI_HTML.read_text(encoding="utf-8"))


class JsSanity(unittest.TestCase):
    def test_adapter_files_are_clean(self):
        for path in (CODE_JS, UI_HTML, MOCKUP_JS):
            with self.subTest(path=path.name):
                self.assertEqual(check_js.check_file(str(path)), [])


class Checker(unittest.TestCase):
    def assertFinds(self, src, fragment):
        findings = check_js.check_source(src)
        self.assertTrue(any(fragment in f for f in findings), findings)

    def test_clean_constructs(self):
        src = r"""
const a = x / 2 / y;
const r = /[/\]]+/g.test(s) ? `t ${ {a: 1}.a } ${`in ${b}`}` : 'q\'s';
async function f() { await g(); [1].map(async (v) => await v); }
const o = { async m() { await f(); }, n: () => ({ a: 1 }) };
const h = async (v) => { if (v) { return await v; } };
"""
        self.assertEqual(check_js.check_source(src), [])

    def test_unbalanced(self):
        self.assertFinds("function a() { if (x) { }", "never closed")
        self.assertFinds("const a = [1, 2);", "closes '['")
        self.assertFinds("a());", "unexpected ')'")

    def test_unterminated(self):
        self.assertFinds("const s = 'abc;\n", "unterminated string")
        self.assertFinds("const t = `abc", "unterminated template")
        self.assertFinds("/* x", "unterminated /*")

    def test_slips(self):
        self.assertFinds("const a = True;", "looks like Python")
        self.assertFinds("f(a,, b);", "doubled comma")
        self.assertFinds("function f() {}\nfunction f() {}", "declared again")
        self.assertFinds("function f() { await g(); }", "outside an async function")
        self.assertFinds("const f = async () => [1].map((v) => await v);", "outside an async function")


if __name__ == "__main__":
    unittest.main()

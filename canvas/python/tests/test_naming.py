import json
import tempfile
import unittest
from pathlib import Path

import support
from dwocanvas import naming
from dwocanvas.naming import apply_naming, load_rules, match_prefix, resolve, sanitize_widget_name
from dwocanvas.schema import validate_schema


class Sanitize(unittest.TestCase):
    def test_cases(self):
        cases = {
            "Apply": "Apply", "Apply Button": "Apply_Button", "a--b..c": "a_b_c", "__x__": "x", "a__b": "a_b",
            "1st": "W_1st", "": "Widget", "___": "Widget", "제목 없음": "Widget", "버튼 OK": "OK", "Row-2": "Row_2",
        }
        for raw, expected in cases.items():
            with self.subTest(raw=raw):
                self.assertEqual(sanitize_widget_name(raw), expected)


class Prefixes(unittest.TestCase):
    def test_default_table(self):
        expected = {
            "btn_": ("button", True), "list_": ("list", True), "tile_": ("tile", True), "scroll_": ("scroll", True),
            "input_": ("textInput", True), "chk_": ("checkBox", True), "slider_": ("slider", True),
            "progress_": ("progressBar", True), "switch_": ("switcher", True), "slot_": ("namedSlot", True),
            "spacer_": ("spacer", False), "_": ("ignore", False),
        }
        got = {r["prefix"]: (r["role"], r["variable"]) for r in load_rules()["prefixes"]}
        self.assertEqual(got, expected)
        self.assertEqual(load_rules()["variablePrefix"], "$")

    def test_resolve(self):
        self.assertEqual(resolve("btn_Apply"), {"role": "button", "variable": True, "widgetName": "Apply", "dollar": False})
        self.assertEqual(resolve("spacer_Gap")["variable"], False)
        self.assertEqual(resolve("spacer_Gap")["role"], "spacer")
        self.assertEqual(resolve("_Guide")["role"], "ignore")
        self.assertFalse(resolve("_Guide")["variable"])
        self.assertEqual(resolve("$Label"), {"role": None, "variable": True, "widgetName": "Label", "dollar": True})
        self.assertEqual(resolve("Title"), {"role": None, "variable": False, "widgetName": "Title", "dollar": False})
        self.assertEqual(resolve("btn_$Ok")["widgetName"], "Ok")
        self.assertEqual(resolve("btn_1")["widgetName"], "W_1")
        self.assertEqual(resolve("Btn_Apply")["role"], None)  # case-sensitive

    def test_longest_prefix_wins(self):
        rules = {"variablePrefix": "$", "prefixes": [
            {"prefix": "_", "role": "ignore", "variable": False},
            {"prefix": "btn_", "role": "button", "variable": True},
            {"prefix": "btn_icon_", "role": "x-iconButton", "variable": True}]}
        self.assertEqual(match_prefix("btn_icon_Close", rules)["role"], "x-iconButton")
        self.assertEqual(resolve("btn_icon_Close", rules)["widgetName"], "Close")
        self.assertEqual(match_prefix("btn_Close", rules)["role"], "button")
        self.assertEqual(match_prefix("_btn_Close", rules)["role"], "ignore")

    def test_custom_rules_file(self):
        rules = {"variablePrefix": "@", "prefixes": [{"prefix": "b-", "role": "button", "variable": False}]}
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rules.json"
            path.write_text(json.dumps(rules), encoding="utf-8")
            loaded = load_rules(path)
        self.assertEqual(resolve("b-Ok", loaded), {"role": "button", "variable": False, "widgetName": "Ok", "dollar": False})
        self.assertEqual(resolve("@Name", loaded)["variable"], True)
        self.assertEqual(resolve("btn_Ok", loaded)["role"], None)

    def test_bad_rules(self):
        for bad in ({}, {"prefixes": [{"prefix": "", "role": "button"}]}, {"prefixes": [{"prefix": "a", "role": "b", "variable": "yes"}]}):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                naming.check_rules(bad)


class ApplyNaming(unittest.TestCase):
    def test_fills_missing_fields(self):
        root = support.node("1", "Screen", children=[
            support.node("2", "btn_Apply"), support.node("3", "$Title", "text", text={"content": "x"}),
            support.node("4", "spacer_Gap"), support.node("5", "Plain name")])
        doc = apply_naming(support.v2_doc(root))
        r = doc["documents"][0]["root"]
        self.assertEqual((r.get("role"), r["variable"], r["widgetName"]), (None, False, "Screen"))
        b, t, s, p = r["children"]
        self.assertEqual((b["role"], b["variable"], b["widgetName"]), ("button", True, "Apply"))
        self.assertEqual((t.get("role"), t["variable"], t["widgetName"]), (None, True, "Title"))
        self.assertEqual((s["role"], s["variable"], s["widgetName"]), ("spacer", False, "Gap"))
        self.assertEqual((p.get("role"), p["variable"], p["widgetName"]), (None, False, "Plain_name"))
        self.assertEqual(validate_schema(doc), [])

    def test_existing_fields_win(self):
        root = support.node("1", "btn_Apply", role="x-fancy", widgetName="ApplyButton")
        r = apply_naming(support.v2_doc(root))["documents"][0]["root"]
        self.assertEqual((r["role"], r["variable"], r["widgetName"]), ("x-fancy", True, "ApplyButton"))
        root = support.node("1", "btn_Apply", variable=False)
        r = apply_naming(support.v2_doc(root))["documents"][0]["root"]
        self.assertEqual((r["role"], r["variable"]), ("button", False))
        root = support.node("1", "Gap", role="spacer")
        self.assertFalse(apply_naming(support.v2_doc(root))["documents"][0]["root"]["variable"])

    def test_components_are_named(self):
        comp = {"id": "K", "name": "Row", "root": support.node("K", "Row", "component", children=[support.node("K2", "$Label")])}
        doc = apply_naming(support.v2_doc(components=[comp]))
        self.assertEqual(doc["components"][0]["root"]["children"][0]["widgetName"], "Label")

    def test_idempotent(self):
        root = support.node("1", "Screen", children=[support.node("2", "btn_Apply")])
        once = apply_naming(support.v2_doc(root))
        twice = apply_naming(json.loads(json.dumps(once)))
        self.assertEqual(once, twice)


if __name__ == "__main__":
    unittest.main()

import copy
import unittest

import support
from dwocanvas.migrate import EXTRACT_CAPABILITIES, font_weight, migrate
from dwocanvas.naming import iter_nodes
from dwocanvas.schema import SCHEMA_V2, validate_schema
from dwocanvas.validate import has_errors, validate, validate_document

V1_SAMPLES = ("sample_shop.json", "DwoMockup.dwocanvas.json")


def v1_doc(root, **extra):
    doc = {
        "schema": "designbridge/1",
        "source": {"tool": "figma", "file": "f", "exporter": "dwocanvas-figma/0.1.0"},
        "images": {"img_0": {"data": "AAAA", "scale": 2}, "img_1": {"file": "slices/a.png", "nineSlice": [1, 2, 3, 4]}},
        "documents": [{"id": "D", "name": "Screen", "root": root}],
    }
    doc.update(extra)
    return doc


class FontWeights(unittest.TestCase):
    def test_table(self):
        cases = {
            "Thin": 100, "ExtraLight": 200, "Ultra Light": 200, "Light": 300, "Regular": 400, "Book": 400,
            "Normal": 400, "Medium": 500, "SemiBold": 600, "Semi Bold": 600, "DemiBold": 600, "Bold": 700,
            "ExtraBold": 800, "Extra Bold": 800, "Heavy": 800, "Black": 900, "Condensed": 400,
        }
        for style, weight in cases.items():
            with self.subTest(style=style):
                self.assertEqual(font_weight(style), (weight, False))

    def test_italic(self):
        self.assertEqual(font_weight("Italic"), (400, True))
        self.assertEqual(font_weight("Bold Italic"), (700, True))
        self.assertEqual(font_weight("SemiBoldItalic"), (600, True))
        self.assertEqual(font_weight("Light Italic"), (300, True))


class FieldMapping(unittest.TestCase):
    def test_header(self):
        out = migrate(v1_doc(support.node("1", "Screen")))
        self.assertEqual(out["schema"], SCHEMA_V2)
        self.assertNotIn("images", out)
        self.assertEqual(out["assets"]["img_0"], {"data": "AAAA", "scale": 2})
        self.assertEqual(out["assets"]["img_1"], {"uri": "slices/a.png", "nineSlice": [1, 2, 3, 4]})
        self.assertEqual(out["capabilities"], EXTRACT_CAPABILITIES)
        self.assertEqual(out["source"]["mode"], "extract")
        self.assertEqual(out["source"]["adapter"], "dwocanvas-figma/0.1.0")
        self.assertEqual(out["source"]["tool"], "figma")

    def test_roles(self):
        mapping = {"Button": "button", "ListView": "list", "TileView": "tile", "ScrollBox": "scroll", "TextInput": "textInput",
                   "CheckBox": "checkBox", "Slider": "slider", "ProgressBar": "progressBar", "Switcher": "switcher",
                   "NamedSlot": "namedSlot", "Spacer": "spacer", "Ignore": "ignore", "Auto": "auto"}
        children = [support.node(str(i), "n%d" % i, meta={"role": ue}) for i, ue in enumerate(mapping)]
        out = migrate(v1_doc(support.node("r", "Screen", children=children)))
        got = {ue: c.get("role") for ue, c in zip(mapping, out["documents"][0]["root"]["children"])}
        self.assertEqual(got, mapping)
        by_role = {c["role"]: c for c in out["documents"][0]["root"]["children"]}
        self.assertTrue(by_role["button"]["variable"])
        self.assertFalse(by_role["spacer"]["variable"])
        self.assertNotIn("meta", by_role["button"])  # emptied meta is dropped

    def test_no_role_from_names(self):
        out = migrate(v1_doc(support.node("r", "Screen", children=[support.node("b", "btn_Apply")])))
        child = out["documents"][0]["root"]["children"][0]
        self.assertNotIn("role", child)
        self.assertNotIn("variable", child)
        self.assertNotIn("widgetName", child)

    def test_variable_and_other_meta(self):
        root = support.node("r", "Screen", meta={"variable": "true", "class": "/Script/M.W"}, children=[
            support.node("a", "A", meta={"variable": True}), support.node("b", "B", meta={"variable": "false"})])
        out = migrate(v1_doc(root))
        r = out["documents"][0]["root"]
        self.assertTrue(r["variable"])
        self.assertEqual(r["meta"], {"class": "/Script/M.W"})
        self.assertTrue(r["children"][0]["variable"])
        self.assertNotIn("variable", r["children"][1])

    def test_unknown_meta_role_kept_with_warning(self):
        report = []
        out = migrate(v1_doc(support.node("r", "Screen", meta={"role": "Banana"})), report)
        self.assertEqual(out["documents"][0]["root"]["meta"], {"role": "Banana"})
        self.assertNotIn("role", out["documents"][0]["root"])
        self.assertEqual([i["level"] for i in report], ["warning", "info"])

    def test_paint_token_and_fonts(self):
        root = support.node("r", "Screen", fills=[{"type": "solid", "color": [1, 0, 0, 1], "token": "color/primary"}],
                            states={"hovered": {"fills": [{"type": "solid", "color": [1, 1, 1], "token": "hover"}]}},
                            children=[support.node("t", "Label", "text", text={"content": "Hi", "font": {"family": "Roboto", "style": "SemiBold Italic", "size": 14},
                                                                               "runs": [{"start": 0, "end": 1, "font": {"style": "Black"}}]})])
        out = migrate(v1_doc(root))
        r = out["documents"][0]["root"]
        self.assertEqual(r["fills"][0], {"type": "solid", "color": [1, 0, 0, 1], "colorToken": "color/primary"})
        self.assertEqual(r["states"]["hovered"]["fills"][0]["colorToken"], "hover")
        font = r["children"][0]["text"]["font"]
        self.assertEqual((font["weight"], font["italic"], font["style"]), (600, True, "SemiBold Italic"))
        self.assertEqual(r["children"][0]["text"]["runs"][0]["font"]["weight"], 900)

    def test_unknown_top_level_becomes_extension(self):
        out = migrate(v1_doc(support.node("r", "Screen"), custom={"a": 1}))
        self.assertEqual(out["x-custom"], {"a": 1})
        self.assertEqual(validate_schema(out), [])

    def test_input_not_modified(self):
        doc = v1_doc(support.node("r", "Screen", meta={"role": "Button"}))
        before = copy.deepcopy(doc)
        migrate(doc)
        self.assertEqual(doc, before)

    def test_idempotent(self):
        once = migrate(v1_doc(support.node("r", "Screen", meta={"role": "Button"})))
        self.assertEqual(migrate(once), once)
        v2 = support.v2_doc()
        self.assertEqual(migrate(v2), v2)


class SampleMigration(unittest.TestCase):
    def test_samples_are_v1(self):
        for name in V1_SAMPLES:
            self.assertEqual(support.load_json(support.SAMPLES_DIR / name)["schema"], "designbridge/1")

    def test_migrated_samples_pass_validation(self):
        for name in V1_SAMPLES:
            with self.subTest(name=name):
                migrated = migrate(support.load_json(support.SAMPLES_DIR / name))
                self.assertEqual(validate_schema(migrated), [])
                issues = validate(migrated)
                self.assertFalse(has_errors(issues), [i for i in issues if i["level"] == "error"])
                self.assertEqual(migrate(migrated), migrated)

    def test_validate_migrates_v1_with_info(self):
        for name in V1_SAMPLES:
            with self.subTest(name=name):
                issues, doc = validate_document(support.load_json(support.SAMPLES_DIR / name))
                self.assertEqual(doc["schema"], SCHEMA_V2)
                self.assertTrue(any(i["rule"] == "migration" and i["level"] == "info" for i in issues))
                self.assertFalse(has_errors(issues))

    def test_mockup_details(self):
        doc = migrate(support.load_json(support.SAMPLES_DIR / "DwoMockup.dwocanvas.json"))
        self.assertEqual(doc["source"]["adapter"], "dwocanvas-figma/0.1.0")
        nodes = list(iter_nodes(doc))
        self.assertTrue(all("role" not in n for n in nodes))  # v1 had no meta.role here
        fonts = [n["text"]["font"] for n in nodes if "text" in n]
        self.assertTrue(fonts and all(isinstance(f.get("weight"), int) for f in fonts))
        self.assertTrue(any(n.get("meta", {}).get("class") for n in nodes))
        self.assertEqual(set(doc["assets"]), {"img_0", "img_1", "img_2"})


if __name__ == "__main__":
    unittest.main()

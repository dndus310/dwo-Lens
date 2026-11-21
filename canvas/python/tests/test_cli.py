import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import support
from dwocanvas.__main__ import main


def run(*argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = main(list(argv))
    return code, out.getvalue(), err.getvalue()


class Cli(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, name, doc):
        path = self.dir / name
        path.write_text(json.dumps(doc), encoding="utf-8")
        return str(path)

    def test_validate_clean_v2(self):
        code, out, _ = run("validate", self.write("ok.json", support.v2_doc()))
        self.assertEqual(code, 0)
        self.assertIn("(dwocanvas/2)", out)
        self.assertIn("Summary: 0 error(s), 0 warning(s), 0 info", out)

    def test_validate_errors_exit_1_grouped(self):
        root = support.node("1", "Screen", children=[support.node("2", "A"), support.node("2", "B", widgetName="1x")])
        code, out, _ = run("validate", self.write("bad.json", support.v2_doc(root, capabilities=["layout.flow"])))
        self.assertEqual(code, 1)
        lines = out.splitlines()
        self.assertTrue(lines[1].startswith("ERROR ("))
        self.assertLess(lines.index(next(l for l in lines if l.startswith("ERROR"))),
                        lines.index(next(l for l in lines if l.startswith("INFO"))))
        self.assertIn("[node-id-duplicate]", out)
        self.assertIn("[schema]", out)

    def test_validate_json(self):
        code, out, _ = run("validate", self.write("ok.json", support.v2_doc(capabilities=[])), "--json")
        self.assertEqual(code, 0)
        items = json.loads(out)
        self.assertEqual(items, [{"level": "info", "node": "", "name": "", "message": "No 'stable-ids' capability: re-import preservation not guaranteed", "rule": "stable-ids"}])

    def test_validate_v1_sample(self):
        code, out, _ = run("validate", str(support.SAMPLES_DIR / "sample_shop.json"))
        self.assertEqual(code, 0)
        self.assertIn("(designbridge/1 -> dwocanvas/2)", out)
        self.assertIn("[migration]", out)

    def test_usage_and_io_errors_exit_2(self):
        self.assertEqual(run()[0], 2)
        self.assertEqual(run("frobnicate")[0], 2)
        self.assertEqual(run("validate")[0], 2)
        self.assertEqual(run("validate", str(self.dir / "missing.json"))[0], 2)
        bad = self.dir / "bad.json"
        bad.write_text("{not json", encoding="utf-8")
        code, _, err = run("validate", str(bad))
        self.assertEqual(code, 2)
        self.assertIn("not valid JSON", err)
        self.assertEqual(run("validate", str(support.SAMPLES_DIR / "sample_shop.json"), "--schema-dir", str(self.dir))[0], 2)
        self.assertEqual(run("naming", self.write("ok.json", support.v2_doc()), str(self.dir / "o.json"), "--rules", str(bad))[0], 2)

    def test_migrate(self):
        out_path = self.dir / "shop.v2.json"
        code, out, _ = run("migrate", str(support.SAMPLES_DIR / "sample_shop.json"), str(out_path))
        self.assertEqual(code, 0)
        doc = json.loads(out_path.read_text(encoding="utf-8"))
        self.assertEqual(doc["schema"], "dwocanvas/2")
        self.assertIn("assets", doc)
        code, out, _ = run("validate", str(out_path))
        self.assertEqual(code, 0)
        self.assertNotIn("[migration]", out)

    def test_naming(self):
        root = support.node("1", "Screen", children=[support.node("2", "btn_Ok")])
        out_path = self.dir / "named.json"
        code, _, _ = run("naming", self.write("in.json", support.v2_doc(root)), str(out_path))
        self.assertEqual(code, 0)
        child = json.loads(out_path.read_text(encoding="utf-8"))["documents"][0]["root"]["children"][0]
        self.assertEqual((child["role"], child["variable"], child["widgetName"]), ("button", True, "Ok"))

    def test_naming_custom_rules(self):
        rules = self.write("rules.json", {"prefixes": [{"prefix": "b-", "role": "button", "variable": True}]})
        root = support.node("1", "Screen", children=[support.node("2", "b-Ok"), support.node("3", "btn_Ok")])
        out_path = self.dir / "named.json"
        code, _, _ = run("naming", self.write("in.json", support.v2_doc(root)), str(out_path), "--rules", rules)
        self.assertEqual(code, 0)
        a, b = json.loads(out_path.read_text(encoding="utf-8"))["documents"][0]["root"]["children"]
        self.assertEqual(a["role"], "button")
        self.assertNotIn("role", b)

    def test_module_entry_point(self):
        env = dict(os.environ, PYTHONPATH=str(support.PYTHON_DIR))
        proc = subprocess.run([sys.executable, "-m", "dwocanvas", "validate", str(support.SAMPLES_DIR / "sample_settings.dwocanvas.json"), "--json"],
                              capture_output=True, text=True, encoding="utf-8", env=env)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIsInstance(json.loads(proc.stdout), list)


if __name__ == "__main__":
    unittest.main()

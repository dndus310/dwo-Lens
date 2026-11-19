"""Unit tests for the build lines, the CI export (JUnit/SARIF), retention and the local CI decisions."""
from __future__ import annotations

import json
import shutil
import sys
import tempfile
import time
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import forge_ci  # noqa: E402
import lens_build  # noqa: E402
import lens_export  # noqa: E402

ZERO = "0" * 40
A, B = "a" * 40, "b" * 40


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class Tmp(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="ci-test-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


class BuildParseTests(Tmp):
    LOG = (
        "C:\\P\\Source\\Game\\A.cpp(54,39): error C2039: 'ReplaceChildAt': 'UPanelWidget'의 멤버가 아닙니다.\n"
        "C:\\P\\Source\\Game\\A.cpp(54,39): error C2039: 'ReplaceChildAt': 'UPanelWidget'의 멤버가 아닙니다.\n"
        "C:\\P\\Source\\Game\\B.cpp(10): warning C4996: deprecated\n"
        "Module.Game.obj : error LNK2019: unresolved external symbol foo\n"
        "    FString& OutError = Error;   // source echo, not a diagnostic\n"
        "Result: Failed (OtherCompilationError)\n"
    )

    def test_compile_errors_are_deduplicated(self):
        parsed = lens_build.parse_compile(self.LOG.splitlines())
        self.assertEqual([e["code"] for e in parsed["errors"]], ["C2039", "LNK2019"])
        self.assertEqual(parsed["errors"][0]["line"], 54)
        self.assertEqual(parsed["warnings"], 1)

    def test_compile_line_verdicts(self):
        run = self.tmp / "compile-20260101-000000"
        write(run / "raw" / "compile.log", self.LOG)
        write(run / "raw" / "compile-meta.json", json.dumps({"exitCode": 6, "target": "GameEditor"}))
        self.assertEqual(lens_build.compile_line(run, None)["verdict"], "fail")
        write(run / "raw" / "compile.log", "Result: Succeeded\n")
        write(run / "raw" / "compile-meta.json", json.dumps({"exitCode": 0}))
        self.assertEqual(lens_build.compile_line(run, None)["verdict"], "pass")
        self.assertEqual(lens_build.compile_line(run, "BP only")["skipped"], "BP only")

    def test_package_needs_build_successful(self):
        run = self.tmp / "package-20260101-000000"
        archive = run / "build" / "Windows"
        write(archive / "Game.exe", "x")
        write(run / "raw" / "package-meta.json", json.dumps({"exitCode": 0}))
        write(run / "raw" / "package.log", "LogCook: Display: Done\nBUILD SUCCESSFUL\n")
        ok = lens_build.package_line(run, run / "build")
        self.assertEqual(ok["verdict"], "pass")
        self.assertEqual(ok["executables"], ["Windows\\Game.exe" if sys.platform == "win32" else "Windows/Game.exe"])
        write(run / "raw" / "package.log", "LogCook: Error: missing asset /Game/X\nBUILD SUCCESSFUL\n")
        self.assertEqual(lens_build.package_line(run, run / "build")["verdict"], "warn")
        write(run / "raw" / "package-meta.json", json.dumps({"exitCode": 6}))
        write(run / "raw" / "package.log", "BUILD FAILED\n")
        self.assertEqual(lens_build.package_line(run, run / "build")["verdict"], "fail")


class ExportTests(Tmp):
    def make_run(self, name: str, findings: list[dict]) -> Path:
        run = self.tmp / "runs" / name
        write(run / "run.json", json.dumps({"line": "architecture-report", "final": "warn",
                                            "stations": [{"id": "ST-1", "name": "collect", "verdict": "pass", "ms": 1200}]}))
        write(run / "out" / "findings.json", json.dumps({"findings": findings}))
        return run

    def test_junit_and_sarif_with_baseline(self):
        first = self.make_run("run-20260101-000000", [{"rule": "naming", "severity": "warn", "message": "prefix", "items": ["/Game/A"]}])
        lens_export.export(first, None, None)
        sarif = json.loads((first / "out" / "results.sarif").read_text(encoding="utf-8"))
        self.assertEqual(sarif["version"], "2.1.0")
        self.assertFalse(sarif["runs"][0]["properties"]["baselineCompared"])
        second = self.make_run("run-20260102-000000", [
            {"rule": "naming", "severity": "warn", "message": "prefix", "items": ["/Game/A", "/Game/B"]},
            {"rule": "module-cycle", "severity": "fail", "message": "cycle", "items": ["A -> B"]},
        ])
        copy = self.tmp / "ci-out"
        summary = lens_export.export(second, first / "out" / "results.sarif", copy)
        results = json.loads((second / "out" / "results.sarif").read_text(encoding="utf-8"))["runs"][0]["results"]
        states = {r["locations"][0]["logicalLocations"][0]["fullyQualifiedName"]: r["baselineState"] for r in results}
        self.assertEqual(states, {"/Game/A": "unchanged", "/Game/B": "new", "A -> B": "new"})
        self.assertEqual(summary["new"], 2)
        suite = ET.parse(second / "out" / "junit.xml").getroot().find("testsuite")
        self.assertEqual(suite.get("failures"), "1")   # only the fail-severity rule fails the suite
        self.assertTrue((copy / second.name / "junit.xml").exists())
        self.assertTrue((copy / second.name / "summary.json").exists())

    def test_build_errors_become_results(self):
        run = self.tmp / "runs" / "compile-20260101-000000"
        write(run / "run.json", json.dumps({"line": "compile", "final": "fail", "stations": [{"id": "B1", "verdict": "fail", "exitCode": 6}]}))
        write(run / "out" / "compile.json", json.dumps({"errors": [{"file": "C:\\P\\A.cpp", "line": 3, "code": "C2039", "message": "x"}]}))
        lens_export.export(run, None, None)
        r = json.loads((run / "out" / "results.sarif").read_text(encoding="utf-8"))["runs"][0]["results"][0]
        self.assertEqual(r["ruleId"], "compile/C2039")
        self.assertEqual(r["locations"][0]["physicalLocation"]["region"]["startLine"], 3)


class RetentionTests(Tmp):
    def test_oldest_runs_beyond_the_limits(self):
        runs = self.tmp / "runs"
        for i in range(5):
            (runs / f"logs-2026010{i}-000000").mkdir(parents=True)
            (runs / f"package-2026010{i}-000000").mkdir(parents=True)
        doomed = [d.name for d in forge_ci.prune_plan(runs, keep_per_check=3, keep_packages=2)]
        self.assertEqual(doomed, ["logs-20260100-000000", "logs-20260101-000000",
                                  "package-20260100-000000", "package-20260101-000000", "package-20260102-000000"])


class HookDecisionTests(Tmp):
    def ci(self):
        return forge_ci.ci_config({})

    def test_push_needs_the_pre_push_marker(self):
        state = self.tmp / "state"
        fetched = f"{A} {B} refs/remotes/origin/develop\n"
        self.assertEqual(forge_ci.decide("reference-transaction", fetched, self.ci(), state), [])   # someone else's push
        forge_ci.decide("pre-push", f"refs/heads/develop {B} refs/heads/develop {A}\n", self.ci(), state, remote="origin")
        out = forge_ci.decide("reference-transaction", fetched, self.ci(), state)
        self.assertEqual([(t["event"], t["ref"]) for t in out], [("push", "develop")])
        self.assertEqual(out[0]["stages"], ["compile", "lens", "package"])
        self.assertEqual(forge_ci.decide("reference-transaction", fetched, self.ci(), state), [])   # marker used once

    def test_raw_marker_from_the_shell_hook(self):
        state = self.tmp / "state"
        write(state / forge_ci.PUSH_RAW, f"origin {int(time.time())}\nrefs/heads/feature/x {B} refs/heads/feature/x {ZERO}\n")
        ci = self.ci()
        ci["triggers"]["push"]["branches"] = ["feature/*"]
        out = forge_ci.decide("reference-transaction", f"{ZERO} {B} refs/remotes/origin/feature/x\n", ci, state)
        self.assertEqual([t["ref"] for t in out], ["feature/x"])
        self.assertFalse((state / forge_ci.PUSH_RAW).exists())

    def test_stale_marker_is_ignored(self):
        state = self.tmp / "state"
        forge_ci.decide("pre-push", f"refs/heads/develop {B} refs/heads/develop {A}\n", self.ci(), state, now=1000)
        self.assertEqual(forge_ci.decide("reference-transaction", f"{A} {B} refs/remotes/origin/develop\n", self.ci(), state, now=1000 + 3600), [])

    def test_branch_create_commit_and_merge(self):
        state = self.tmp / "state"
        ci = self.ci()
        created = forge_ci.decide("reference-transaction", f"{ZERO} {A} refs/heads/feature/login\n{ZERO} {A} refs/heads/hotfix/x\n", ci, state)
        self.assertEqual([(t["event"], t["ref"]) for t in created], [("branchCreate", "feature/login")])
        self.assertEqual(forge_ci.decide("post-commit", "", ci, state, branch="feature/login")[0]["stages"], ["lens"])
        self.assertEqual(forge_ci.decide("post-commit", "", ci, state, branch="main"), [])
        self.assertEqual(forge_ci.decide("post-commit", "", ci, state, branch="HEAD"), [])   # detached / rebase
        self.assertEqual(forge_ci.decide("post-merge", "", ci, state, branch="develop")[0]["event"], "merge")

    def test_config_overrides_triggers(self):
        ci = forge_ci.ci_config({"ci": {"keepBuilds": 5, "triggers": {"commit": {"branches": []}}}})
        self.assertEqual(ci["keepBuilds"], 5)
        self.assertEqual(ci["triggers"]["commit"]["branches"], [])
        self.assertEqual(ci["triggers"]["commit"]["stages"], ["lens"])   # untouched keys keep their default


class JobXmlTests(unittest.TestCase):
    def test_job_has_retention_parameters_and_pipeline(self):
        xml = forge_ci.job_xml("C:\\P", "C:\\P\\__dwo-forge.bat", forge_ci.ci_config({"ci": {"keepBuilds": 7, "keepArtifacts": 2}}))
        root = ET.fromstring(xml.encode("utf-8"))
        strategy = root.find("properties/jenkins.model.BuildDiscarderProperty/strategy")
        self.assertEqual(strategy.find("numToKeep").text, "7")
        self.assertEqual(strategy.find("artifactNumToKeep").text, "2")
        # Strict limit: Jenkins otherwise never deletes the last successful/stable build.
        self.assertEqual(strategy.find("removeLastBuild").text, "true")
        names = [p.find("name").text for p in root.find("properties/hudson.model.ParametersDefinitionProperty/parameterDefinitions")]
        self.assertEqual(names, ["REASON", "COMPILE", "LENS", "PACKAGE"])
        script = root.find("definition/script").text
        self.assertIn("DWO_FORGE_LAUNCHER = 'C:\\\\P\\\\__dwo-forge.bat'", script)
        self.assertIn("powershell '& $env:DWO_FORGE_LAUNCHER compile; exit $LASTEXITCODE'", script)
        self.assertIn("for (check in ['analyze', 'logs', 'sizes'])", script)
        self.assertIn("junit allowEmptyResults: true", script)
        self.assertEqual(root.find("definition/sandbox").text, "true")

    def test_step_scripts_carry_no_paths(self):
        # Jenkins writes step scripts as UTF-8 and cmd/PowerShell 5.1 read them in the console code page (949 on Korean
        # Windows): a Korean user or project name must only reach the steps through environment variables.
        launcher = "C:\\Users\\홍길동\\내 프로젝트\\__dwo-forge.bat"
        script = forge_ci.pipeline_script(launcher, ["analyze"])
        self.assertNotIn("bat ", script)
        self.assertNotIn("bat(", script)
        steps = [l for l in script.splitlines() if "powershell" in l]
        self.assertTrue(steps)
        for line in steps:
            self.assertTrue(line.isascii(), line)
        self.assertIn("DWO_FORGE_LAUNCHER = 'C:\\\\Users\\\\홍길동\\\\내 프로젝트\\\\__dwo-forge.bat'", script)

    def test_launcher_quote_is_escaped(self):
        script = forge_ci.pipeline_script("C:\\O'Brien\\__dwo-forge.bat", [])
        self.assertIn("DWO_FORGE_LAUNCHER = 'C:\\\\O\\'Brien\\\\__dwo-forge.bat'", script)


class BuildLogEncodingTests(Tmp):
    def test_utf8_log(self):
        p = self.tmp / "compile.log"
        p.write_bytes("\ufeffC:\\프로젝트\\A.cpp(3): error C2039: 'X': 'Y'의 멤버가 아닙니다.\n".encode("utf-8"))
        errors = lens_build.parse_compile(lens_build.read_lines(p))["errors"]
        self.assertEqual(errors[0]["file"], "C:\\프로젝트\\A.cpp")
        self.assertIn("멤버가 아닙니다", errors[0]["message"])

    def test_console_code_page_log(self):
        # A 949 console writes the log in cp949; it must not come back as replacement characters.
        import codecs
        try:
            if codecs.lookup("oem").name != "cp949":
                self.skipTest("this PC's OEM code page is not 949")
        except LookupError:
            self.skipTest("no OEM code page (not Windows)")
        p = self.tmp / "compile.log"
        p.write_bytes("C:\\프로젝트\\A.cpp(3): error C2039: 멤버가 아닙니다\n".encode("cp949"))
        self.assertIn("프로젝트", lens_build.read_lines(p)[0])

    def test_undecodable_bytes_do_not_crash(self):
        p = self.tmp / "package.log"
        p.write_bytes(b"BUILD SUCCESSFUL\n\xff\xfe\xfa broken\n")
        self.assertEqual(lens_build.read_lines(p)[0], "BUILD SUCCESSFUL")


class BundleTests(Tmp):
    def test_manifest_round_trip_and_tamper(self):
        write(self.tmp / "jenkins.war", "war")
        write(self.tmp / "plugins" / "junit.jpi", "plugin")
        forge_ci.make_manifest(self.tmp, {"jenkinsVersion": "2.0"})
        self.assertEqual(forge_ci.verify_bundle(self.tmp)["jenkinsVersion"], "2.0")
        write(self.tmp / "plugins" / "junit.jpi", "changed")
        with self.assertRaises(ValueError):
            forge_ci.verify_bundle(self.tmp)


if __name__ == "__main__":
    unittest.main()

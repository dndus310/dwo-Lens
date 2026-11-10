"""Unit tests for the dwo-Forge Lens Python tools. Standard library only.

    python -m unittest discover -s tests -v
"""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
FIXTURE = Path(__file__).resolve().parent / "fixtures" / "raw_temp_proj"
sys.path.insert(0, str(TOOLS))

import lens_analyze as analyze  # noqa: E402
import lens_logs as logs  # noqa: E402
import lens_quality as quality  # noqa: E402
import lens_runtime as runtime  # noqa: E402
import lens_verify as verify  # noqa: E402


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


class TempDir(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="lens-test-"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)


# =============================================================================== log parser

class LogParserTests(TempDir):
    LOG = "\n".join([
        "[2026.10.27-16.40.57:829][  0]LogInit: Display: starting",
        "[2026.10.27-16.40.58:001][  1]LogBlueprint: Error: [Compiler] Missing pin on node 12 in /Game/BP_A",
        "[2026.10.27-16.40.58:002][  1]LogBlueprint: Error: [Compiler] Missing pin on node 99 in /Game/BP_A",
        "[2026.10.27-16.40.58:002][  1]LogBlueprint: Error: [Compiler] Missing pin on node 7 in /Game/BP_B",
        "[2026.10.27-16.40.58:003][  2]LogTemp: Warning: Slow frame 33.4 ms",
        "LogContentValidation: Warning: Asset C:\\Proj\\Content\\X.uasset has issues",
        "[2026.10.27-16.40.58:004][  3]LogOutputDevice: Ensure condition failed: Ptr != nullptr",
        "[2026.10.27-16.40.58:005][  3]LogWindows: Fatal: Assertion failed: Index < Num",
        "  0x00007ff6 UnrealEditor-Core.dll!FOo::Bar()",
        "  0x00007ff7 UnrealEditor-Engine.dll!UWorld::Tick()",
        "",
        "[2026.10.27-16.40.58:006][  4]LogConfig: Warning: ignored category line",
    ])

    def parse(self, ignore=frozenset({"LogConfig"})):
        return logs.parse_file(write(self.tmp / "a.log", self.LOG), set(ignore))

    def test_severities_and_categories(self):
        events = self.parse()
        kinds = [(e["severity"], e["category"]) for e in events]
        self.assertIn(("Error", "LogBlueprint"), kinds)
        self.assertIn(("Warning", "LogContentValidation"), kinds)  # line without timestamp
        self.assertIn(("Error", "LogOutputDevice"), kinds)  # ensure without explicit severity
        self.assertIn(("Fatal", "LogWindows"), kinds)
        self.assertNotIn("LogConfig", [e["category"] for e in events])  # ignored category
        self.assertNotIn("LogInit", [e["category"] for e in events])  # Display is not a failure

    def test_fatal_captures_call_stack(self):
        fatal = [e for e in self.parse() if e["severity"] == "Fatal"][0]
        self.assertEqual(len(fatal["detail"]), 2)
        self.assertIn("UWorld::Tick", fatal["detail"][1])

    def test_fingerprint_ignores_numbers_and_paths(self):
        a = logs.fingerprint("LogX", "Missing pin on node 12 at C:\\A\\b.uasset 0xDEADBEEF")
        b = logs.fingerprint("LogX", "Missing pin on node 99 at D:\\Z\\q.uasset 0x00000001")
        self.assertEqual(a, b)
        self.assertNotEqual(a, logs.fingerprint("LogY", "Missing pin on node 12 at C:\\A\\b.uasset 0xDEADBEEF"))

    def test_grouping_and_baseline(self):
        summary = logs.summarize(self.parse(), [])
        blueprint = sorted((g for g in summary["groups"] if g["category"] == "LogBlueprint"), key=lambda g: -g["count"])
        # Same error on the same asset groups together; a different asset is a different problem.
        self.assertEqual([g["count"] for g in blueprint], [2, 1])
        self.assertEqual(summary["groups"][0]["severity"], "Fatal")  # fatal sorts first
        again = logs.summarize(self.parse(), [], baseline=summary)
        self.assertEqual(again["newGroups"], 0)
        fewer = logs.summarize([e for e in self.parse() if e["severity"] != "Fatal"], [])
        with_new = logs.summarize(self.parse(), [], baseline=fewer)
        self.assertEqual(with_new["newGroups"], 1)

    def test_verdicts(self):
        empty = logs.summarize([], [])
        self.assertEqual(logs.verdict_of(empty), "pass")
        warn_only = logs.summarize([e for e in self.parse() if e["severity"] == "Warning"], [])
        self.assertEqual(logs.verdict_of(warn_only), "warn")
        errors = [e for e in self.parse() if e["severity"] == "Error"]
        self.assertEqual(logs.verdict_of(logs.summarize(errors, [])), "warn")  # no baseline: not new
        base = logs.summarize([], [])
        self.assertEqual(logs.verdict_of(logs.summarize(errors, [], baseline=base)), "fail")  # new errors
        self.assertEqual(logs.verdict_of(logs.summarize(errors, [], baseline=base), warn_on_new=False), "warn")
        self.assertEqual(logs.verdict_of(logs.summarize(self.parse(), [])), "fail")  # fatal

    def test_crash_context(self):
        crash = self.tmp / "Crashes" / "UECC-1"
        write(crash / "CrashContext.runtime-xml",
              "<FGenericCrashContext><RuntimeProperties><ErrorMessage>Access violation</ErrorMessage>"
              "<CallStack>A()\nB()\n</CallStack><EngineVersion>5.7.4</EngineVersion></RuntimeProperties></FGenericCrashContext>")
        write(self.tmp / "Crashes" / "UECC-2" / "CrashContext.runtime-xml", "<broken")
        crashes = logs.parse_crashes(self.tmp / "Crashes")
        self.assertEqual(len(crashes), 2)
        self.assertEqual(crashes[0]["error"], "Access violation")
        self.assertEqual(crashes[0]["callstack"], ["A()", "B()"])
        self.assertIn("unreadable", crashes[1]["error"])
        self.assertEqual(logs.verdict_of(logs.summarize([], crashes)), "fail")


# =============================================================================== analyzer

class AnalyzerHelperTests(unittest.TestCase):
    def test_normalize_skeleton_class(self):
        self.assertEqual(analyze.normalize_class("/Game/W.SKEL_W_C"), "/Game/W.W_C")
        self.assertEqual(analyze.normalize_class("/Script/UMG.UserWidget"), "/Script/UMG.UserWidget")

    def test_collapse_external_actor_packages(self):
        roots = ["/Game"]
        self.assertEqual(analyze.collapse_package("/Game/__ExternalActors__/Maps/L1/5/1A/ABC", roots), "/Game/Maps/L1")
        self.assertEqual(analyze.collapse_package("/Game/BP_A", roots), "/Game/BP_A")

    def test_tarjan_cycles(self):
        cycles = analyze.tarjan({"a": {"b"}, "b": {"c"}, "c": {"a"}, "d": {"a"}})
        self.assertEqual(cycles, [["a", "b", "c"]])
        self.assertEqual(analyze.tarjan({"a": {"b"}, "b": set()}), [])

    def test_reach_counts(self):
        graph = {"a": {"b"}, "b": {"c"}, "c": {"a"}, "d": {"a"}, "e": set()}
        self.assertEqual(analyze.reach_counts(graph, ["a", "b", "c", "d", "e"]), {"a": 2, "b": 2, "c": 2, "d": 3, "e": 0})
        long_chain = {str(i): {str(i + 1)} for i in range(3000)}
        counts = analyze.reach_counts(long_chain, ["0", "2999"])
        self.assertEqual(counts, {"0": 3000, "2999": 1})

    def test_tarjan_deep_chain_has_no_recursion_error(self):
        chain = {str(i): {str(i + 1)} for i in range(5000)}
        self.assertEqual(analyze.tarjan(chain), [])


class AnalyzerFixtureTests(TempDir):
    """Runs the full analyzer on real data extracted from a Blueprint-only project."""

    def run_cli(self, out: Path) -> Path:
        cmd = [sys.executable, str(TOOLS / "lens_analyze.py"), "--raw", str(FIXTURE),
               "--project", str(FIXTURE / "temp_proj.uproject"), "--out", str(out), "--run-id", "test"]
        subprocess.run(cmd, check=True, capture_output=True)
        return out

    def test_graph_contents(self):
        out = self.run_cli(self.tmp / "out")
        graph = json.loads((out / "graph.json").read_text(encoding="utf-8"))
        keys = {s["key"] for s in graph["symbols"]}
        self.assertIn("bp:/Game/WBP_Item.WBP_Item_C", keys)
        inherits = {(r["source"], r["target"]) for r in graph["relations"] if r["type"] == "Inherits"}
        self.assertIn(("bp:/Game/WBP_Item.WBP_Item_C", "uclass:/Script/UMG.UserWidget"), inherits)
        hard = {(r["source"], r["target"]) for r in graph["relations"] if r["type"] == "AssetHardRef"}
        self.assertIn(("asset:/Game/WBP_ListView", "asset:/Game/WBP_Item"), hard)
        self.assertTrue(all(r["status"] in ("verified", "rule", "inferred", "unknown") for r in graph["relations"]))
        for name in ("report.html", "index.db", "class_uml.puml", "findings.json", "quality.json", "statemonitor.json"):
            self.assertTrue((out / name).exists(), name)
        self.assertTrue(list((out / "flows").glob("*.mmd")))

    def test_deterministic_output(self):
        a = self.run_cli(self.tmp / "a")
        b = self.run_cli(self.tmp / "b")
        for name in ("graph.json", "findings.json", "quality.json", "class_uml.mmd"):
            ha = hashlib.sha256((a / name).read_bytes()).hexdigest()
            hb = hashlib.sha256((b / name).read_bytes()).hexdigest()
            self.assertEqual(ha, hb, f"{name} differs between identical runs")

    def test_sqlite_index(self):
        import sqlite3
        out = self.run_cli(self.tmp / "out")
        db = sqlite3.connect(out / "index.db")
        (count,) = db.execute("SELECT COUNT(*) FROM relation WHERE relation_type = 'Inherits'").fetchone()
        db.close()
        self.assertGreater(count, 0)


class ModuleScanTests(TempDir):
    def test_build_cs_dependencies_and_cycle(self):
        write(self.tmp / "P.uproject", json.dumps({"Modules": [{"Name": "Game"}]}))
        write(self.tmp / "Source/Game/Game.Build.cs",
              'PublicDependencyModuleNames.AddRange(new string[] { "Core", "Ui" });\n'
              '// PrivateDependencyModuleNames.Add("Commented");\n')
        write(self.tmp / "Source/Ui/Ui.Build.cs", 'PrivateDependencyModuleNames.Add("Game");\n')
        g = analyze.Graph()
        deps = analyze.scan_modules(self.tmp / "P.uproject", g)
        self.assertEqual(deps["Game"], {"Core", "Ui"})
        self.assertNotIn("Commented", deps["Game"])
        self.assertTrue(g.symbols["module:Core"]["external"])
        self.assertFalse(g.symbols["module:Ui"]["external"])
        ctx = {"module_deps": deps, "asset_hard": {}, "meta": {}, "tick_users": [], "asset_class": {}}
        result = analyze.run_checks(g, ctx)
        self.assertEqual(result["verdict"], "fail")
        self.assertEqual(result["findings"][0]["rule"], "module-cycle")


# =============================================================================== quality checks

class QualityTests(TempDir):
    def ctx(self, **extra):
        base = {"roots": ["/Game"], "asset_class": {}, "asset_hard": {}, "asset_soft": {},
                "bp_index": {}, "project_file": write(self.tmp / "P.uproject", "{}")}
        base.update(extra)
        return base

    def test_load_config_merges_sections(self):
        a = write(self.tmp / "a.json", json.dumps({"naming": {"forbidSpaces": True, "prefixes": {"Blueprint": ["BP_"]}}}))
        b = write(self.tmp / "b.json", json.dumps({"naming": {"forbidSpaces": False}}))
        cfg = quality.load_config([a, b, self.tmp / "missing.json"])
        self.assertFalse(cfg["naming"]["forbidSpaces"])
        self.assertEqual(cfg["naming"]["prefixes"]["Blueprint"], ["BP_"])

    def test_naming_rules(self):
        cfg = {"naming": {"prefixes": {"Blueprint": ["BP_"], "World": []}, "forbidAssetsAtContentRoot": True,
                          "forbidSpaces": True, "forbidNonAsciiNames": True, "ignoreFolders": ["/Game/Developers"]}}
        ctx = self.ctx(asset_class={
            "/Game/Chars/BP_Hero": "/Script/Engine.Blueprint",
            "/Game/Chars/Hero": "/Script/Engine.Blueprint",
            "/Game/BP_Root": "/Script/Engine.Blueprint",
            "/Game/Maps/My Map": "/Script/Engine.World",
            "/Game/Maps/맵": "/Script/Engine.World",
            "/Game/Developers/x/Thing": "/Script/Engine.Blueprint",
        })
        findings, data = quality.check_naming(ctx, cfg)
        bad = {v["asset"]: v["problems"] for v in data["violations"]}
        self.assertNotIn("/Game/Chars/BP_Hero", bad)
        self.assertIn("/Game/Chars/Hero", bad)
        self.assertIn("콘텐츠 루트에 직접 위치", bad["/Game/BP_Root"])
        self.assertIn("이름에 공백", bad["/Game/Maps/My Map"])
        self.assertIn("이름에 비ASCII 문자", bad["/Game/Maps/맵"])
        self.assertNotIn("/Game/Developers/x/Thing", bad)
        self.assertEqual(findings[0]["severity"], "warn")

    def test_unused_assets_and_redirectors(self):
        write(self.tmp / "Config/DefaultEngine.ini", "GameDefaultMap=/Game/Maps/Start.Start\n")
        ctx = self.ctx(asset_class={
            "/Game/Maps/Start": "/Script/Engine.World",
            "/Game/Maps/Other": "/Script/Engine.World",
            "/Game/UI/W_Hud": "/Script/UMGEditor.WidgetBlueprint",
            "/Game/UI/W_Old": "/Script/UMGEditor.WidgetBlueprint",
            "/Game/UI/W_Font": "/Script/Engine.Font",
            "/Game/UI/Redir": "/Script/CoreUObject.ObjectRedirector",
        }, asset_hard={"/Game/Maps/Start": {"/Game/UI/W_Hud"}}, asset_soft={"/Game/UI/W_Hud": {"/Game/UI/W_Font"}})
        findings, data = quality.check_unused(ctx, {"unused": {}})
        self.assertEqual(data["unused"], ["/Game/UI/W_Old"])
        self.assertEqual(data["redirectors"], ["/Game/UI/Redir"])
        self.assertIn("/Game/Maps/Start", data["iniRoots"])
        self.assertEqual({f["rule"] for f in findings}, {"unused-asset", "redirector"})

    def test_complexity_handles_cycles_and_thresholds(self):
        nodes = [{"id": str(i), "class": "K2Node_CallFunction"} for i in range(6)]
        nodes.append({"id": "b", "class": "K2Node_IfThenElse"})
        links = [{"from": str(i), "to": str(i + 1), "kind": "exec"} for i in range(5)]
        links.append({"from": "5", "to": "0", "kind": "exec"})  # loop back
        bp = {"asset": "/Game/BP_Loop.BP_Loop", "graphs": [{"name": "EventGraph", "kind": "event", "nodes": nodes, "links": links}]}
        findings, data = quality.check_complexity(self.ctx(bp_index={"bp:x": bp}),
                                                  {"complexity": {"warnNodesPerGraph": 5, "warnBranchesPerGraph": 5, "warnExecChainLength": 100}})
        row = data["graphs"][0]
        self.assertEqual(row["nodes"], 7)
        self.assertEqual(row["branches"], 1)
        self.assertEqual(row["execChain"], 6)
        self.assertEqual(row["over"], ["노드 수"])
        self.assertEqual(findings[0]["rule"], "bp-complexity")

    def test_state_monitor_detection(self):
        write(self.tmp / "P.uproject", json.dumps({"Plugins": [{"Name": "StateMonitor", "Enabled": True}]}))
        write(self.tmp / "Plugins/StateMonitor/StateMonitor.uplugin", "{}")
        write(self.tmp / "Plugins/__DwoLensInspector/StateMonitor.uplugin", "{}")  # never counted
        write(self.tmp / "Source/Game/Hud.cpp",
              '#include "StateMonitorSubsystem.h"\n'
              'void F(UObject* W) {\n'
              '  UStateMonitorSubsystem::Get(W)->Submit(TEXT("Player"), State);\n'
              '  UStateMonitorLibrary::SubmitStructSnapshot(W, TEXT("Wave"), FText(), T, &D);\n'
              '}\n')
        bp = {"asset": "/Game/BP_A.BP_A", "graphs": [{"name": "EventGraph", "nodes": [
            {"id": "n1", "role": "call", "member": "SubmitStateSnapshot", "memberOwner": "/Script/StateMonitor.StateMonitorLibrary"}]}]}
        findings, data = quality.check_state_monitor(self.ctx(bp_index={"bp:a": bp}), {"stateMonitor": {}})
        self.assertTrue(data["present"] and data["enabled"])
        self.assertEqual(data["sourceIds"], ["Player", "Wave"])
        self.assertEqual(len([r for r in data["cppRefs"] if r["submit"]]), 2)
        self.assertEqual(len(data["bpCalls"]), 1)
        self.assertEqual(data["testFilter"], "StateMonitor.")
        self.assertEqual(findings[0]["rule"], "state-monitor")

    def test_dwo_state_monitor_detection(self):
        # dwo-Forge installs the renamed plugin as Plugins/__DwoStateMonitor; BP nodes reference /Script/DwoStateMonitor.
        write(self.tmp / "P.uproject", json.dumps({"Plugins": []}))
        write(self.tmp / "Plugins/__DwoStateMonitor/DwoStateMonitor.uplugin", "{}")
        bp = {"asset": "/Game/BP_A.BP_A", "graphs": [{"name": "EventGraph", "nodes": [
            {"id": "n1", "role": "call", "member": "SubmitStateSnapshot", "memberOwner": "/Script/DwoStateMonitor.StateMonitorLibrary"},
            {"id": "n2", "role": "call", "member": "WatchStateObject", "memberOwner": "/Script/StateMonitor.StateMonitorLibrary"}]}]}
        cfg = json.loads((ROOT / "config" / "default.json").read_text(encoding="utf-8"))
        findings, data = quality.check_state_monitor(self.ctx(bp_index={"bp:a": bp}), cfg)
        self.assertTrue(data["present"] and data["enabled"])
        self.assertEqual(data["plugin"].replace("\\", "/"), "Plugins/__DwoStateMonitor/DwoStateMonitor.uplugin")
        self.assertEqual(len(data["bpCalls"]), 2, "assets saved against the old module still count")
        self.assertEqual(data["testFilter"], "StateMonitor.")

    def test_state_monitor_absent(self):
        findings, data = quality.check_state_monitor(self.ctx(), {"stateMonitor": {}})
        self.assertFalse(data["present"])
        self.assertIsNone(data["testFilter"])
        self.assertEqual(findings, [])


# =============================================================================== verify line

class VerifyTests(TempDir):
    def automation(self, tests):
        index = {"succeeded": sum(t["state"] == "Success" for t in tests), "succeededWithWarnings": 0,
                 "failed": sum(t["state"] == "Fail" for t in tests), "notRun": 0, "tests": tests}
        path = write(self.tmp / "run/automation/index.json", "")
        path.write_bytes(b"\xef\xbb\xbf" + json.dumps(index).encode("utf-8"))  # UE writes a BOM

    def test_worst_ignores_info(self):
        self.assertEqual(verify.worst(["pass", "info"]), "pass")
        self.assertEqual(verify.worst(["pass", "warn", "fail"]), "fail")
        self.assertEqual(verify.worst([]), "pass")

    def test_all_pass(self):
        write(self.tmp / "run/compile.log", "[2026.10.27-16.40.57:829][  0]LogBlueprint: Display: ok\n")
        write(self.tmp / "run/datavalidation.log", "")
        self.automation([{"fullTestPath": "Project.A", "state": "Success", "entries": []}])
        result = verify.verify(self.tmp / "run", None)
        self.assertEqual(result["verdict"], "pass")
        self.assertEqual([s["verdict"] for s in result["steps"]], ["pass", "pass", "pass"])

    def test_failed_test_and_compile_error(self):
        write(self.tmp / "run/compile.log", "[2026.10.27-16.40.57:829][  0]LogBlueprint: Error: [Compiler] broken\n")
        self.automation([{"fullTestPath": "Project.A", "state": "Fail",
                          "entries": [{"event": {"type": "Error", "message": "expected 1"}}]}])
        result = verify.verify(self.tmp / "run", None)
        steps = {s["id"]: s for s in result["steps"]}
        self.assertEqual(steps["V1"]["verdict"], "warn")  # errors without a baseline are reported, not new
        self.assertEqual(steps["V2"]["verdict"], "skip")
        self.assertEqual(steps["V3"]["verdict"], "fail")
        self.assertEqual(steps["V3"]["automation"]["tests"][0]["errors"], ["expected 1"])
        self.assertEqual(result["verdict"], "fail")

    def test_no_matching_tests_is_info(self):
        self.automation([])
        result = verify.verify(self.tmp / "run", None)
        self.assertEqual({s["id"]: s["verdict"] for s in result["steps"]}["V3"], "info")
        self.assertEqual(result["verdict"], "pass")

    def test_automation_log_without_report_fails(self):
        write(self.tmp / "run/automation.log", "crashed before writing the report")
        result = verify.verify(self.tmp / "run", None)
        self.assertEqual({s["id"]: s["verdict"] for s in result["steps"]}["V3"], "fail")


# =============================================================================== runtime lines

class PerfCsvTests(TempDir):
    def csv(self, frames):
        rows = ["EVENTS,FrameTime,GameThreadTime,RenderThreadTime,RHIThreadTime"]
        rows += [f",{ft},{ft / 2},{ft / 3},1.0" for ft in frames]
        rows += ["EVENTS,FrameTime,GameThreadTime,RenderThreadTime,RHIThreadTime",
                 "[HasHeaderRowAtEnd],1,[platform],Windows,[config],Development"]
        return write(self.tmp / "p.csv", "\n".join(rows) + "\n")

    def test_reads_rows_until_trailing_header_and_metadata(self):
        cols = runtime.read_profile_csv(self.csv([10, 20, 30]))
        self.assertEqual(cols["FrameTime"], [10.0, 20.0, 30.0])
        self.assertEqual(cols["RHIThreadTime"], [1.0, 1.0, 1.0])
        self.assertNotIn("EVENTS", cols)  # text-only column dropped

    def test_stats_percentiles_and_hitches(self):
        frames = [10.0] * 90 + [20.0] * 8 + [150.0, 200.0]
        cfg = {"perf": {"warmupFrames": 0, "budgetMs": 33.3, "hitchMs": 100}}
        r = runtime.perf(self.csv(frames), "/Game/M", cfg)
        ft = r["stats"]["FrameTime"]
        self.assertEqual((ft["p50"], ft["p95"], ft["p99"], ft["max"]), (10.0, 20.0, 150.0, 200.0))
        self.assertAlmostEqual(ft["avg"], 14.1, places=2)
        self.assertEqual([h["frame"] for h in r["hitches"]], [98, 99])
        self.assertEqual(r["verdict"], "warn")  # p95 within budget, hitches present
        self.assertEqual(r["frames"], 100)
        self.assertIn("GameThreadTime", r["stats"])

    def test_warmup_is_excluded_and_p95_over_budget_fails(self):
        r = runtime.perf(self.csv([500, 500] + [40] * 20), "/Game/M", {"perf": {"warmupFrames": 2, "hitchMs": 100}})
        self.assertEqual(r["frames"], 20)
        self.assertEqual(r["hitches"], [])
        self.assertEqual(r["verdict"], "fail")

    def test_missing_csv_fails(self):
        self.assertEqual(runtime.perf(None, "/Game/M", {})["verdict"], "fail")

    def test_downsample_keeps_peaks(self):
        values = [1.0] * 1200
        values[777] = 99.0
        out = runtime.downsample(values, 600)
        self.assertEqual(len(out), 600)
        self.assertIn(99.0, out)


class SizesTests(TempDir):
    def test_closure_weights_follow_hard_refs_and_cycles(self):
        graph = {"/Game/A": {"/Game/B"}, "/Game/B": {"/Game/C"}, "/Game/C": {"/Game/B"}, "/Game/D": set()}
        sizes = {"/Game/A": 1, "/Game/B": 10, "/Game/C": 100, "/Game/D": 1000}
        w = runtime.closure_weights(graph, sizes)
        self.assertEqual(w["/Game/A"], (111, 2))
        self.assertEqual(w["/Game/B"], (110, 1))  # B and C load each other
        self.assertEqual(w["/Game/D"], (1000, 0))

    def test_scan_folds_external_actors_and_ranks(self):
        c = self.tmp / "proj/Content"
        for rel, n in (("Maps/L_Main.umap", 100), ("__ExternalActors__/Maps/L_Main/0/AB/X1.uasset", 50),
                       ("Props/SM_Rock.uasset", 30), ("Props/SM_Rock.ubulk", 300), ("Props/T_Rock.uasset", 20),
                       ("readme.txt", 999)):
            (c / rel).parent.mkdir(parents=True, exist_ok=True)
            (c / rel).write_bytes(b"x" * n)
        graph = {"relations": [
            {"source": "asset:/Game/Maps/L_Main", "target": "asset:/Game/Props/SM_Rock", "type": "AssetHardRef"},
            {"source": "asset:/Game/Props/SM_Rock", "target": "asset:/Game/Props/T_Rock", "type": "AssetHardRef"},
            {"source": "asset:/Game/Props/T_Rock", "target": "asset:/Game/Maps/L_Main", "type": "AssetSoftRef"}]}
        r = runtime.sizes(self.tmp / "proj", {"sizes": {"warnAssetMB": 200 / 1048576}}, graph)
        self.assertEqual(r["totalBytes"], 500)
        self.assertEqual(r["assetCount"], 3)
        self.assertEqual(r["largest"][0], {"asset": "/Game/Props/SM_Rock", "bytes": 330})
        self.assertEqual(r["largest"][1], {"asset": "/Game/Maps/L_Main", "bytes": 150})
        self.assertEqual(r["heaviestClosures"][0], {"asset": "/Game/Maps/L_Main", "closureBytes": 500, "closureCount": 2})
        self.assertEqual({t["ext"] for t in r["byType"]}, {".umap", ".uasset", ".ubulk"})
        self.assertEqual(r["verdict"], "warn")
        self.assertEqual({o["asset"] for o in r["overLimit"]}, {"/Game/Props/SM_Rock"})
        self.assertEqual({f["folder"] for f in r["byFolder"]}, {"/Game/Maps", "/Game/Props"})

    def test_list_maps_skips_excluded_and_external(self):
        c = self.tmp / "proj/Content"
        for rel in ("Maps/A.umap", "Developers/me/Test.umap", "__ExternalActors__/Maps/A/0/X.umap", "B.umap"):
            write(c / rel, "")
        maps = runtime.list_maps(self.tmp / "proj", {"smoke": {"exclude": ["/Game/Developers/*"], "maxMaps": 20}})
        self.assertEqual(maps, ["/Game/B", "/Game/Maps/A"])
        self.assertEqual(runtime.list_maps(self.tmp / "proj", {"smoke": {"maxMaps": 1}}), ["/Game/B"])


class SmokeTests(TempDir):
    def run_smoke(self, log_text, **entry):
        log = write(self.tmp / "run/raw/smoke-1.log", log_text)
        e = {"map": "/Game/M", "log": str(log), "exitCode": None, "killed": True, "seconds": 15, **entry}
        write(self.tmp / "run/raw/smoke-runs.json", json.dumps(e))  # a single map arrives as an object
        return runtime.smoke(self.tmp / "run", self.tmp / "proj", {"smoke": {"maxErrors": 0, "maxWarnings": 1}})

    def test_clean_boot_passes(self):
        r = self.run_smoke("[2026.10.27-16.40.57:829][  0]LogInit: Display: ok\n")
        self.assertEqual((r["verdict"], r["maps"][0]["verdict"]), ("pass", "pass"))
        self.assertIsNone(r["maps"][0]["exitCode"])

    def test_ensure_fails(self):
        r = self.run_smoke("[2026.10.27-16.40.58:001][  1]LogOutputDevice: Warning: Ensure condition failed: x [File:a.cpp]\n")
        self.assertEqual(r["maps"][0]["ensures"], 1)
        self.assertEqual(r["verdict"], "fail")

    def test_errors_over_threshold_fail_and_warnings_warn(self):
        self.assertEqual(self.run_smoke("LogTemp: Error: boom\n")["verdict"], "fail")
        r = self.run_smoke("LogTemp: Warning: a\nLogTemp: Warning: b\n")
        self.assertEqual(r["verdict"], "warn")
        self.assertEqual(r["maps"][0]["warnings"], 2)

    def test_crash_exit_and_fatal(self):
        self.assertEqual(self.run_smoke("", exitCode=3, killed=False)["verdict"], "fail")
        r = self.run_smoke("[2026.10.27-16.40.58:001][  1]LogWindows: Error: === Critical error: ===\n")
        self.assertTrue(r["maps"][0]["fatal"])
        self.assertEqual(r["verdict"], "fail")

    def test_verdict_rules(self):
        base = {"errors": 0, "warnings": 0, "ensures": 0, "fatal": False, "killed": True, "exitCode": None}
        self.assertEqual(runtime.smoke_verdict(base, 0, 10)[0], "pass")
        self.assertEqual(runtime.smoke_verdict({**base, "errors": 2}, 5, 10)[0], "warn")
        self.assertEqual(runtime.smoke_verdict({**base, "errors": 6}, 5, 10)[0], "fail")
        self.assertEqual(runtime.smoke_verdict({**base, "killed": False, "exitCode": 0}, 0, 10)[0], "warn")
        self.assertEqual(runtime.smoke_verdict({**base, "logFound": False}, 0, 10)[0], "fail")


class CatalogAndConfigTests(unittest.TestCase):
    def test_catalog_is_consistent(self):
        catalog = json.loads((TOOLS / "catalog.json").read_text(encoding="utf-8"))
        ids = [t["id"] for t in catalog["tools"]]
        self.assertEqual(len(ids), len(set(ids)), "duplicate tool ids")
        for t in catalog["tools"]:
            self.assertIn(t["kind"], ("build", "wrap", "ai"))
            self.assertIn(t["status"], ("ready", "planned"))
            self.assertRegex(t["station"], r"^(ST-[1-5]|OPS|UI)$")
            self.assertIn(t["tool"], ("lens", "statemonitor", "canvas", "forge"))
        tools = {t["tool"] for t in catalog["tools"]}
        self.assertEqual(tools, {"lens", "statemonitor", "canvas", "forge"}, "every dwo-Forge tool group has catalog entries")
        commands = " ".join(t["command"] for t in catalog["tools"])
        for command in ("canvas-install", "canvas-remove", "canvas-import", "canvas-validate", "sm-install", "sm-remove"):
            self.assertIn(command, commands)

    def test_default_config_is_valid_json_with_all_sections(self):
        cfg = json.loads((ROOT / "config" / "default.json").read_text(encoding="utf-8"))
        for section in ("naming", "complexity", "unused", "verify", "logs", "smoke", "perf", "sizes", "trace", "stateMonitor"):
            self.assertIn(section, cfg)


if __name__ == "__main__":
    unittest.main()

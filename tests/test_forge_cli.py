"""Command-line tests of tools/forge.ps1 that need no Unreal Engine: migration from dwo-Lens 1.x, the git
exclude block, DwoCanvas install-state rules, and uninstall. Every test works on a throwaway project with
LOCALAPPDATA redirected, so the real %LOCALAPPDATA%\\dwo-Forge is never touched. Windows only.
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FORGE = ROOT / "tools" / "forge.ps1"
LEGACY_LAUNCHER = ROOT / "tests" / "fixtures" / "legacy_1x" / "__dwo-lens.bat"
HAS_POWERSHELL = sys.platform == "win32" and shutil.which("powershell") is not None
HAS_GIT = shutil.which("git") is not None

LEGACY_BLOCK = ("# >>> dwo-lens managed: . (do not edit) >>>\n/__dwo-lens/\n/__dwo-lens.bat\n/Plugins/__DwoLensInspector/\n"
                "/Plugins/__StateMonitor/\n# <<< dwo-lens managed <<<\n")


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


@unittest.skipUnless(HAS_POWERSHELL, "needs Windows PowerShell")
class ForgeCliTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="forge-cli-"))
        self.project = self.tmp / "Game"
        self.appdata = self.tmp / "appdata"
        self.appdata.mkdir()
        write(self.project / "Game.uproject", json.dumps({"FileVersion": 3, "EngineAssociation": "5.7", "Plugins": []}))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def env(self, **extra):
        e = dict(os.environ)
        e["LOCALAPPDATA"] = str(self.appdata)
        for k in ("DWO_FORGE_FROM_BAT", "DWO_LENS_FROM_BAT"):
            e.pop(k, None)
        e.update(extra)
        return e

    def forge(self, command, *rest, **env):
        args = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(FORGE),
                "-Command", command, "-Project", str(self.project), *rest]
        r = subprocess.run(args, capture_output=True, text=True, encoding="utf-8", errors="replace", env=self.env(**env))
        return r.returncode, r.stdout + r.stderr

    def git(self, *args):
        return subprocess.run(["git", "-C", str(self.project), *args], capture_output=True, text=True).stdout

    def make_legacy_project(self):
        """A project as dwo-Lens 1.x left it: __dwo-lens folder, launcher, exclude block, AppData folder."""
        lens = self.project / "__dwo-lens"
        write(lens / "state.json", json.dumps({"schema": "lens.state/0.2", "project": "Game", "gitStatusBeforeAttach": ""}))
        write(lens / "home.txt", str(ROOT))
        write(lens / "data" / "runs" / "run-20260101-000000" / "run.json", json.dumps({"line": "architecture-report", "final": "pass"}))
        write(lens / "gui.json", json.dumps({"mutes": ["finding:x"]}))
        shutil.copy(LEGACY_LAUNCHER, self.project / "__dwo-lens.bat")
        write(self.appdata / "dwo-Lens" / "gui.json", json.dumps({"recent": [str(self.project)]}))
        write(self.appdata / "dwo-Lens" / "uninstalled" / "Old-1" / "data" / "x.txt", "kept history")
        if HAS_GIT:
            subprocess.run(["git", "init", "-q", str(self.project)], check=True)
            write(self.project / ".gitignore", "Saved/\nIntermediate/\n")
            self.git("add", "Game.uproject", ".gitignore")
            self.git("-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q", "-m", "base")
            exclude = self.project / ".git" / "info" / "exclude"
            exclude.write_text(exclude.read_text(encoding="utf-8") + "\n" + LEGACY_BLOCK, encoding="utf-8")

    # ------------------------------------------------------------------ migration

    def test_migrates_a_dwo_lens_project_on_any_command(self):
        self.make_legacy_project()
        code, out = self.forge("sm-remove")   # needs no engine; migration runs before every command
        self.assertEqual(code, 0, out)
        forge = self.project / "__dwo-forge"
        self.assertTrue((forge / "state.json").exists(), out)
        self.assertTrue((forge / "data" / "runs" / "run-20260101-000000" / "run.json").exists())
        self.assertEqual(json.loads((forge / "gui.json").read_text(encoding="utf-8"))["mutes"], ["finding:x"])
        self.assertEqual((forge / "home.txt").read_text(encoding="utf-8").strip(), str(ROOT))
        self.assertFalse((self.project / "__dwo-lens").exists())
        self.assertFalse((self.project / "__dwo-lens.bat").exists())
        self.assertIn("dwo-Forge project launcher", (self.project / "__dwo-forge.bat").read_text(encoding="utf-8"))
        # %LOCALAPPDATA%\dwo-Lens -> dwo-Forge
        self.assertTrue((self.appdata / "dwo-Forge" / "gui.json").exists())
        self.assertTrue((self.appdata / "dwo-Forge" / "uninstalled" / "Old-1" / "data" / "x.txt").exists())
        self.assertFalse((self.appdata / "dwo-Lens").exists())
        if HAS_GIT:
            exclude = (self.project / ".git" / "info" / "exclude").read_text(encoding="utf-8")
            self.assertIn("# >>> dwo-forge managed: . (do not edit) >>>", exclude)
            self.assertNotIn("dwo-lens managed", exclude)
            self.assertIn("/__dwo-forge/", exclude)
            self.assertEqual(self.git("status", "--porcelain", "--untracked-files=all"), "", "dwo-Forge files stay hidden")
        # a second run changes nothing
        code, out = self.forge("sm-remove")
        self.assertEqual(code, 0, out)
        self.assertNotIn("Migrated", out)

    def test_old_launcher_hands_over_to_the_new_one(self):
        """The 1.x launcher is running while it migrates: it must replace itself with a forwarder, not break."""
        self.make_legacy_project()
        legacy = self.project / "__dwo-lens.bat"
        r = subprocess.run(["cmd", "/c", str(legacy), "sm-remove"], capture_output=True, text=True, encoding="utf-8",
                           errors="replace", env=self.env(), cwd=str(self.project))
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertTrue((self.project / "__dwo-forge" / "state.json").exists())
        self.assertTrue((self.project / "__dwo-forge.bat").exists())
        self.assertIn("dwo-Forge legacy launcher", legacy.read_text(encoding="utf-8"), "old launcher replaced by the forwarder")
        # next use: forwards to __dwo-forge.bat, then the forwarder and the empty __dwo-lens folder are gone
        r = subprocess.run(["cmd", "/c", str(legacy), "sm-remove"], capture_output=True, text=True, encoding="utf-8",
                           errors="replace", env=self.env(), cwd=str(self.project))
        self.assertIn("deprecated", r.stdout)
        self.assertFalse(legacy.exists())
        self.assertFalse((self.project / "__dwo-lens").exists())
        self.assertTrue((self.project / "__dwo-forge.bat").exists())

    def test_uninstall_removes_new_and_old_names(self):
        self.make_legacy_project()
        code, out = self.forge("uninstall", "--delete-data")
        self.assertIn(code, (0, 3), out)
        for name in ("__dwo-forge", "__dwo-forge.bat", "__dwo-lens", "__dwo-lens.bat"):
            self.assertFalse((self.project / name).exists(), f"{name} left behind\n{out}")
        if HAS_GIT:
            exclude = (self.project / ".git" / "info" / "exclude").read_text(encoding="utf-8")
            self.assertNotIn("managed", exclude)

    def test_legacy_state_monitor_is_reported_until_update(self):
        self.make_legacy_project()
        sm = self.project / "Plugins" / "__StateMonitor"
        write(sm / "StateMonitor.uplugin", "{}")
        write(sm / ".dwo-lens-managed", "x")
        code, out = self.forge("sm-remove", "--force")
        self.assertEqual(code, 0, out)
        self.assertFalse(sm.exists(), "sm-remove also removes the dwo-Lens 1.x install (marker of 1.x accepted)")

    # ------------------------------------------------------------------ DwoCanvas

    def make_canvas(self, managed=True):
        plugin = self.project / "Plugins" / "DwoCanvas"
        write(plugin / "DwoCanvas.uplugin", json.dumps({"FileVersion": 3, "VersionName": "0.2.0"}))
        if managed:
            write(plugin / ".dwo-forge-installed", "installed by dwo-Forge")
        return plugin

    def test_canvas_remove_refuses_while_widgets_use_the_runtime(self):
        plugin = self.make_canvas()
        asset = self.project / "Content" / "UI" / "WBP_Shop.uasset"
        asset.parent.mkdir(parents=True)
        asset.write_bytes(b"\xc1\x83\x2a\x9e" + b"\x00" * 64 + b"/Script/DwoCanvasRuntime\x00DwoCanvasFrame\x00" + b"\x00" * 64)
        code, out = self.forge("canvas-remove")
        self.assertNotEqual(code, 0, out)
        self.assertIn("WBP_Shop.uasset", out)
        self.assertTrue(plugin.exists())
        code, out = self.forge("canvas-remove", "--force")
        self.assertEqual(code, 0, out)
        self.assertFalse(plugin.exists())

    def test_canvas_remove_without_users(self):
        plugin = self.make_canvas()
        write(self.project / "Content" / "Maps" / "Main.umap", "no canvas here")
        code, out = self.forge("canvas-remove")
        self.assertEqual(code, 0, out)
        self.assertFalse(plugin.exists())

    def test_canvas_remove_leaves_the_projects_own_copy(self):
        plugin = self.make_canvas(managed=False)
        code, out = self.forge("canvas-remove")
        self.assertNotEqual(code, 0, out)
        self.assertTrue(plugin.exists())

    def test_uninstall_keeps_dwo_canvas(self):
        self.make_legacy_project()
        plugin = self.make_canvas()
        code, out = self.forge("uninstall", "--delete-data")
        self.assertIn(code, (0, 3), out)
        self.assertTrue(plugin.exists())
        self.assertIn("DwoCanvas", out)
        self.assertFalse((self.project / "__dwo-forge").exists())

    def test_canvas_import_needs_the_plugin_and_a_file(self):
        code, out = self.forge("canvas-import")
        self.assertNotEqual(code, 0)
        self.assertIn("Usage", out)
        design = write(self.tmp / "x.dwocanvas.json", json.dumps({"schema": "dwocanvas/2", "documents": []}))
        code, out = self.forge("canvas-import", str(design))
        self.assertNotEqual(code, 0)
        self.assertIn("canvas-install", out)

    def test_canvas_validate(self):
        sample = ROOT / "plugin" / "DwoCanvas" / "Samples" / "sample_settings.dwocanvas.json"
        if not sample.exists():
            self.skipTest("DwoCanvas samples not present")
        code, out = self.forge("canvas-validate", str(sample))
        if not (ROOT / "canvas" / "python" / "dwocanvas").exists():
            self.assertNotEqual(code, 0)
            self.assertIn("validator is not available", out)
        else:
            self.assertNotIn("Traceback", out)
            self.assertIn(code, (0, 1), out)


if __name__ == "__main__":
    unittest.main()

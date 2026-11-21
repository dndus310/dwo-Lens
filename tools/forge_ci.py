"""Local CI helpers used by ci.ps1 and forge.ps1. Pure decisions live here so they can be unit tested.

    python forge_ci.py decide   --event <hook> --config <json...> --state <dir> [--branch B] [--remote R] < stdin
    python forge_ci.py job-xml  --project <dir> --launcher <bat> --name <job> --config <json...>
    python forge_ci.py verify-bundle --dir <extracted bundle>
    python forge_ci.py prune    --runs <runs dir> --config <json...> [--dry-run]

decide prints a JSON list of triggers: [{"event": "push", "ref": "develop", "stages": [...], "reason": "..."}].
"""
from __future__ import annotations

import argparse
import fnmatch
import hashlib
import json
import shutil
import sys
import time
from pathlib import Path
from xml.sax.saxutils import escape

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lens_quality import load_config  # noqa: E402

ZERO = "0" * 40
STAGES = ("compile", "lens", "package")
DEFAULT_CI = {
    "port": 8098,
    "jobPrefix": "dwo-forge",
    "keepBuilds": 30,
    "keepArtifacts": 10,
    "quietPeriodSeconds": 10,
    "lensChecks": ["analyze", "logs", "sizes"],
    "triggers": {
        "push": {"branches": ["develop"], "stages": ["compile", "lens", "package"]},
        "merge": {"branches": ["develop"], "stages": ["compile", "lens"]},
        "branchCreate": {"branches": ["feature/*"], "stages": ["compile", "lens"]},
        "commit": {"branches": ["develop", "feature/*"], "stages": ["lens"]},
        "external": {"stages": ["compile", "lens", "package"]},
    },
}
DEFAULT_RETENTION = {"runsPerCheck": 30, "packages": 3, "minFreeDiskGB": 20}
PUSH_MARKER = "push-pending.json"
PUSH_RAW = "push-pending.raw"
MARKER_MAX_AGE = 15 * 60


def ci_config(cfg: dict) -> dict:
    ci = json.loads(json.dumps(DEFAULT_CI))
    user = cfg.get("ci") or {}
    for k, v in user.items():
        if k == "triggers" and isinstance(v, dict):
            for t, tv in v.items():
                ci["triggers"].setdefault(t, {}).update(tv or {})
        elif not k.startswith("_"):
            ci[k] = v
    return ci


def retention_config(cfg: dict) -> dict:
    return {**DEFAULT_RETENTION, **{k: v for k, v in (cfg.get("retention") or {}).items() if not k.startswith("_")}}


def matches(branch: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(branch, p) for p in patterns or [])


def stages_of(ci: dict, event: str) -> list[str]:
    return [s for s in (ci["triggers"].get(event, {}).get("stages") or []) if s in STAGES]


# ----------------------------------------------------------------------------------------------- hook decisions

def decide(event: str, stdin: str, ci: dict, state: Path, branch: str = "", remote: str = "", now: float | None = None) -> list[dict]:
    """Which builds a git hook should start. Only the user's own actions count: a fetch that moves a
    remote-tracking branch is not a push, so pre-push leaves a marker that the later ref update must match."""
    now = time.time() if now is None else now
    trig = ci["triggers"]
    out: list[dict] = []
    marker_path = state / PUSH_MARKER

    if event == "pre-push":
        refs = []
        for line in stdin.splitlines():
            parts = line.split()
            if len(parts) == 4 and parts[1] != ZERO and parts[2].startswith("refs/heads/"):
                refs.append({"remoteRef": parts[2], "sha": parts[1]})
        if refs:
            state.mkdir(parents=True, exist_ok=True)
            marker_path.write_text(json.dumps({"remote": remote or "origin", "at": now, "refs": refs}), encoding="utf-8")
        return out

    if event == "post-commit":
        if branch and branch != "HEAD" and matches(branch, trig.get("commit", {}).get("branches")):
            out.append({"event": "commit", "ref": branch, "stages": stages_of(ci, "commit"), "reason": f"commit on {branch}"})
        return out

    if event == "post-merge":
        if branch and matches(branch, trig.get("merge", {}).get("branches")):
            out.append({"event": "merge", "ref": branch, "stages": stages_of(ci, "merge"), "reason": f"merge into {branch}"})
        return out

    if event == "reference-transaction":
        marker = None
        raw = state / PUSH_RAW
        if raw.exists():
            # Written by the pre-push hook itself: "<remote> <epoch>" then git's pre-push stdin lines.
            lines = raw.read_text(encoding="utf-8", errors="replace").splitlines()
            raw.unlink(missing_ok=True)
            if lines:
                head = lines[0].split()
                decide("pre-push", "\n".join(lines[1:]), ci, state, remote=head[0] if head else "origin",
                       now=float(head[1]) if len(head) > 1 and head[1].isdigit() else now)
        if marker_path.exists():
            try:
                marker = json.loads(marker_path.read_text(encoding="utf-8"))
            except ValueError:
                marker = None
            if marker and now - float(marker.get("at", 0)) > MARKER_MAX_AGE:
                marker = None
        changed = False
        for line in stdin.splitlines():
            parts = line.split()
            if len(parts) != 3:
                continue
            old, new, ref = parts
            if ref.startswith("refs/heads/") and old == ZERO and new != ZERO:
                b = ref[len("refs/heads/"):]
                if matches(b, trig.get("branchCreate", {}).get("branches")):
                    out.append({"event": "branchCreate", "ref": b, "stages": stages_of(ci, "branchCreate"), "reason": f"branch {b} created"})
            elif ref.startswith("refs/remotes/") and marker and new != ZERO:
                rest = ref[len("refs/remotes/"):]
                rem, _, b = rest.partition("/")
                if rem != marker.get("remote"):
                    continue
                for entry in list(marker.get("refs", [])):
                    if entry["remoteRef"] == f"refs/heads/{b}" and entry["sha"] == new:
                        marker["refs"].remove(entry)
                        changed = True
                        if matches(b, trig.get("push", {}).get("branches")):
                            out.append({"event": "push", "ref": b, "stages": stages_of(ci, "push"),
                                        "reason": f"push to {rem}/{b} ({new[:8]})"})
        if changed:
            if marker["refs"]:
                marker_path.write_text(json.dumps(marker), encoding="utf-8")
            else:
                marker_path.unlink(missing_ok=True)
        return out
    return out


# ----------------------------------------------------------------------------------------------- Jenkins job

def pipeline_script(launcher: str, lens_checks: list[str]) -> str:
    """Declarative pipeline. Runs in the project folder through its __dwo-forge.bat; results are copied into
    the workspace (DWO_FORGE_CI_OUT) for junit/archiveArtifacts. DwoLens fail -> UNSTABLE, build errors -> FAILURE.

    Steps use `powershell`, not `bat`, and keep every path out of the script text: Jenkins writes .bat files as UTF-8
    while cmd reads them in the console code page (949 on Korean Windows), so a non-ASCII workspace or project path
    (e.g. a Korean user name) broke every bat step. Paths travel in environment variables instead."""
    groovy_launcher = launcher.replace("\\", "\\\\").replace("'", "\\'")
    checks = ", ".join(f"'{c}'" for c in lens_checks)
    run = "& $env:DWO_FORGE_LAUNCHER {0}; exit $LASTEXITCODE"
    return f"""pipeline {{
  agent any
  options {{ disableConcurrentBuilds() }}
  environment {{
    DWO_FORGE_LAUNCHER = '{groovy_launcher}'
    DWO_FORGE_CI_OUT = "${{env.WORKSPACE}}\\\\results"
  }}
  stages {{
    stage('준비') {{
      steps {{
        script {{ currentBuild.description = params.REASON }}
        dir('results') {{ deleteDir() }}
      }}
    }}
    stage('C++ 컴파일') {{
      when {{ expression {{ params.COMPILE }} }}
      steps {{ powershell '{run.format("compile")}' }}
    }}
    stage('DwoLens 검사') {{
      when {{ expression {{ params.LENS }} }}
      steps {{
        script {{
          for (check in [{checks}]) {{
            def rc = powershell(returnStatus: true, script: '& $env:DWO_FORGE_LAUNCHER ' + check + '; exit $LASTEXITCODE')
            if (rc == 2) {{ unstable("DwoLens ${{check}}: 실패 판정") }}
            else if (rc != 0) {{ error("DwoLens ${{check}}: 실행 오류 (종료 코드 ${{rc}})") }}
          }}
        }}
      }}
    }}
    stage('패키지 빌드') {{
      when {{ expression {{ params.PACKAGE }} }}
      steps {{ powershell '{run.format("package")}' }}
    }}
  }}
  post {{
    always {{
      junit allowEmptyResults: true, testResults: 'results/**/junit.xml'
      archiveArtifacts allowEmptyArchive: true, artifacts: 'results/**'
    }}
  }}
}}
"""


def job_xml(project: str, launcher: str, ci: dict) -> str:
    script = pipeline_script(launcher, [c for c in ci.get("lensChecks", []) if c])
    params = [("REASON", "string", "manual")] + [(s.upper(), "bool", "true") for s in STAGES]
    param_xml = []
    for name, kind, default in params:
        if kind == "string":
            param_xml.append(f"<hudson.model.StringParameterDefinition><name>{name}</name><defaultValue>{escape(default)}</defaultValue>"
                             "<trim>true</trim></hudson.model.StringParameterDefinition>")
        else:
            param_xml.append(f"<hudson.model.BooleanParameterDefinition><name>{name}</name><defaultValue>{default}</defaultValue>"
                             "</hudson.model.BooleanParameterDefinition>")
    return f"""<?xml version='1.1' encoding='UTF-8'?>
<flow-definition plugin="workflow-job">
  <description>{escape(f"dwo-Forge 로컬 CI: {project}. dwo-Forge가 관리합니다 (__dwo-forge.bat ci-job).")}</description>
  <keepDependencies>false</keepDependencies>
  <properties>
    <jenkins.model.BuildDiscarderProperty>
      <strategy class="hudson.tasks.LogRotator">
        <daysToKeep>-1</daysToKeep><numToKeep>{int(ci['keepBuilds'])}</numToKeep>
        <artifactDaysToKeep>-1</artifactDaysToKeep><artifactNumToKeep>{int(ci['keepArtifacts'])}</artifactNumToKeep>
        <removeLastBuild>true</removeLastBuild>
      </strategy>
    </jenkins.model.BuildDiscarderProperty>
    <hudson.model.ParametersDefinitionProperty><parameterDefinitions>{''.join(param_xml)}</parameterDefinitions></hudson.model.ParametersDefinitionProperty>
  </properties>
  <definition class="org.jenkinsci.plugins.workflow.cps.CpsFlowDefinition" plugin="workflow-cps">
    <script>{escape(script)}</script>
    <sandbox>true</sandbox>
  </definition>
  <quietPeriod>{int(ci['quietPeriodSeconds'])}</quietPeriod>
  <disabled>false</disabled>
</flow-definition>
"""


# ----------------------------------------------------------------------------------------------- bundle

def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_bundle(root: Path) -> dict:
    """Checks every file listed in manifest.json against its SHA-256. Returns the manifest or raises."""
    manifest = json.loads((root / "manifest.json").read_text(encoding="utf-8-sig"))
    if manifest.get("schema") != "dwo-forge.ci-bundle/1":
        raise ValueError("manifest.json is not a dwo-Forge CI bundle")
    bad = [rel for rel, digest in manifest.get("files", {}).items()
           if not (root / rel).is_file() or sha256(root / rel) != digest]
    if bad:
        raise ValueError(f"{len(bad)} file(s) missing or damaged, e.g. {bad[0]}")
    if not manifest.get("files"):
        raise ValueError("manifest.json lists no files")
    return manifest


def make_manifest(root: Path, info: dict) -> dict:
    files = {str(p.relative_to(root)).replace("\\", "/"): sha256(p)
             for p in sorted(root.rglob("*")) if p.is_file() and p.name != "manifest.json"}
    manifest = {"schema": "dwo-forge.ci-bundle/1", **info, "files": files}
    (root / "manifest.json").write_text(json.dumps(manifest, indent=1), encoding="utf-8")
    return manifest


# ----------------------------------------------------------------------------------------------- retention

def prune_plan(runs: Path, keep_per_check: int, keep_packages: int) -> list[Path]:
    """Oldest-first run folders beyond the limits. Package runs have their own (smaller) limit because each
    holds a full packaged build. Run ids sort by time within a prefix (prefix-YYYYMMDD-HHMMSS)."""
    groups: dict[str, list[Path]] = {}
    for d in runs.iterdir() if runs.exists() else []:
        if d.is_dir() and "-" in d.name:
            groups.setdefault(d.name.split("-")[0], []).append(d)
    doomed: list[Path] = []
    for prefix, dirs in groups.items():
        limit = keep_packages if prefix == "package" else keep_per_check
        if limit and limit > 0 and len(dirs) > limit:
            doomed += sorted(dirs, key=lambda d: d.name)[:len(dirs) - limit]
    return sorted(doomed, key=lambda d: d.name)


def free_gb(path: Path) -> float:
    return shutil.disk_usage(path).free / (1 << 30)


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("decide")
    d.add_argument("--event", required=True)
    d.add_argument("--config", nargs="+", type=Path, default=[])
    d.add_argument("--state", type=Path, required=True)
    d.add_argument("--branch", default="")
    d.add_argument("--remote", default="")
    d.add_argument("--input", type=Path)
    j = sub.add_parser("job-xml")
    j.add_argument("--project", required=True)
    j.add_argument("--launcher", required=True)
    j.add_argument("--config", nargs="+", type=Path, default=[])
    j.add_argument("--out", type=Path, required=True)
    v = sub.add_parser("verify-bundle")
    v.add_argument("--dir", type=Path, required=True)
    m = sub.add_parser("make-manifest")
    m.add_argument("--dir", type=Path, required=True)
    m.add_argument("--info-file", type=Path, required=True, help="JSON file whose object is merged into the manifest")
    p = sub.add_parser("prune")
    p.add_argument("--runs", type=Path, required=True)
    p.add_argument("--config", nargs="+", type=Path, default=[])
    p.add_argument("--dry-run", action="store_true")
    c = sub.add_parser("config")
    c.add_argument("--config", nargs="+", type=Path, default=[])
    args = ap.parse_args()

    if args.cmd == "decide":
        stdin = args.input.read_text(encoding="utf-8", errors="replace") if args.input else sys.stdin.read()
        triggers = decide(args.event, stdin, ci_config(load_config(args.config)), args.state, args.branch, args.remote)
        print(json.dumps(triggers, ensure_ascii=False))
    elif args.cmd == "job-xml":
        args.out.write_text(job_xml(args.project, args.launcher, ci_config(load_config(args.config))), encoding="utf-8")
    elif args.cmd == "verify-bundle":
        try:
            manifest = verify_bundle(args.dir)
        except (OSError, ValueError) as exc:
            print(f"[dwo-forge] ERROR: bundle check failed: {exc}")
            return 1
        print(json.dumps({k: v for k, v in manifest.items() if k != "files"} | {"fileCount": len(manifest["files"])}))
    elif args.cmd == "make-manifest":
        manifest = make_manifest(args.dir, json.loads(args.info_file.read_text(encoding="utf-8-sig")))
        print(f"[dwo-forge] manifest: {len(manifest['files'])} files")
    elif args.cmd == "prune":
        r = retention_config(load_config(args.config))
        doomed = prune_plan(args.runs, int(r["runsPerCheck"]), int(r["packages"]))
        for dpath in doomed:
            if not args.dry_run:
                shutil.rmtree(dpath, ignore_errors=True)
        if doomed:
            print(f"[dwo-forge] retention: removed {len(doomed)} old run(s) (keep {r['runsPerCheck']} per check, {r['packages']} packages)")
    elif args.cmd == "config":
        cfg = load_config(args.config)
        print(json.dumps({"ci": ci_config(cfg), "retention": retention_config(cfg)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())

# dwo-Forge self verification. Runs everything a release must pass and writes .qa\selftest\selftest-result.json.
#
#   powershell -File tests\selftest.ps1 -Project <a UE project folder to copy for the integration cycle>
#
# The given project is never modified: the integration cycle runs on an isolated copy under .qa\.
param(
    [string]$Project = '',
    [switch]$SkipIntegration,
    # Copies for the no-git and parent-repository cycles must sit outside every git repository.
    [string]$Scratch = (Join-Path $env:SystemDrive 'dwo-forge-selftest')
)
# Continue: native tools write progress to stderr, which 'Stop' would turn into a terminating error.
# Every step is judged explicitly by Check instead.
$ErrorActionPreference = 'Continue'
$repo = Split-Path -Parent $PSScriptRoot
$qa = Join-Path $repo '.qa\selftest'
if (Test-Path $qa) { Remove-Item -Recurse -Force $qa }
New-Item -ItemType Directory -Force $qa | Out-Null
$checks = New-Object System.Collections.Generic.List[object]
function Check([string]$Group, [string]$Name, [bool]$Ok, [string]$Detail = '', [double]$Seconds = 0) {
    $checks.Add([ordered]@{ group = $Group; name = $Name; ok = $Ok; detail = $Detail; seconds = [math]::Round($Seconds, 1) })
    $mark = if ($Ok) { 'ok  ' } else { 'FAIL' }
    Write-Host ("[selftest] [{0}] {1,-12} {2} {3}" -f $mark, $Group, $Name, $Detail)
}
$sw = [Diagnostics.Stopwatch]::new()

# ---------------------------------------------------------------- 1. unit tests
$sw.Restart()
$unit = & python -m unittest discover -s (Join-Path $repo 'tests') 2>&1 | Out-String
$ran = if ($unit -match 'Ran (\d+) tests') { $Matches[1] } else { '?' }
Check 'unit' 'python unittest' ($LASTEXITCODE -eq 0) "$ran tests" $sw.Elapsed.TotalSeconds

# ---------------------------------------------------------------- 2. static checks
foreach ($ps in (Get-ChildItem -Recurse (Join-Path $repo 'tools'), (Join-Path $repo 'tests') -Filter *.ps1)) {
    $errors = $null
    [System.Management.Automation.Language.Parser]::ParseFile($ps.FullName, [ref]$null, [ref]$errors) | Out-Null
    Check 'static' "parse $($ps.Name)" ($errors.Count -eq 0) "$($errors.Count) errors"
}
foreach ($bat in (Get-ChildItem $repo, (Join-Path $repo 'tools'), (Join-Path $repo 'tests') -Filter *.bat)) {
    $bytes = [IO.File]::ReadAllBytes($bat.FullName)
    $lf = 0; $crlf = 0
    for ($i = 0; $i -lt $bytes.Length; $i++) { if ($bytes[$i] -eq 10) { if ($i -gt 0 -and $bytes[$i - 1] -eq 13) { $crlf++ } else { $lf++ } } }
    Check 'static' "crlf $($bat.Name)" ($lf -eq 0) "LF-only lines: $lf"
}
foreach ($json in @('config\default.json', 'tools\catalog.json')) {
    try { Get-Content -Raw -Encoding UTF8 (Join-Path $repo $json) | ConvertFrom-Json | Out-Null; Check 'static' "json $json" $true }
    catch { Check 'static' "json $json" $false $_.Exception.Message }
}

# ---------------------------------------------------------------- 3. GUI build
$sw.Restart()
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'tools\gui.ps1') -SystemDir $repo -BuildOnly | Out-Null
$guiExe = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'dwo-Forge\gui-cache') -Recurse -Filter dwo-forge-gui.exe -ErrorAction SilentlyContinue |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1
Check 'gui' 'build' ($LASTEXITCODE -eq 0 -and $guiExe -ne $null) "$($guiExe.FullName)" $sw.Elapsed.TotalSeconds

# ---------------------------------------------------------------- 4. integration cycle on an isolated copy
if (-not $SkipIntegration -and $Project) {
    $copy = Join-Path $qa 'project'
    New-Item -ItemType Directory -Force $copy | Out-Null
    Get-ChildItem $Project | Where-Object { $_.Name -notin @('Intermediate', 'Saved', 'DerivedDataCache', '.git', '.vs', '__dwo-forge', '__dwo-forge.bat', '__dwo-lens', '__dwo-lens.bat') -and $_.Name -notlike '*.html' -and $_.Extension -ne '.sln' } |
        ForEach-Object { Copy-Item -Recurse $_.FullName $copy }
    Get-ChildItem -Recurse -Directory -Path (Join-Path $copy 'Plugins') -Filter '__*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    # A DwoCanvas copy in the source project is its own; the cycle installs the bundled one.
    if (Test-Path (Join-Path $copy 'Plugins\DwoCanvas')) { Remove-Item -Recurse -Force (Join-Path $copy 'Plugins\DwoCanvas') }
    & git -C $copy init -q
    # Standard UE ignores so editor work folders do not count as project changes.
    Set-Content -Encoding ASCII (Join-Path $copy '.gitignore') "Saved/`nIntermediate/`nDerivedDataCache/`nBinaries/`n.vs/"
    & git -C $copy add -A
    & git -C $copy -c user.name=selftest -c user.email=selftest@local commit -q -m baseline
    $before = (& git -C $copy status --porcelain) -join "`n"
    $env:DWO_FORGE_HOME = $null
    $env:DWO_LENS_HOME = $null
    $launcher = Join-Path $repo 'dwo-Forge.bat'
    $bat = Join-Path $copy '__dwo-forge.bat'
    function Run([string]$Cmd, [string]$Target = $copy) {
        $sw.Restart()
        # attach goes through dwo-Forge.bat (as a user would); everything after through the project launcher.
        $line = if ($Cmd -like 'attach*') { "`"$launcher`" $Cmd `"$Target`"" } else { "`"$(Join-Path $Target '__dwo-forge.bat')`" $Cmd" }
        $out = & cmd /c $line 2>&1 | Out-String
        $code = $LASTEXITCODE
        [IO.File]::AppendAllText((Join-Path $qa 'integration.log'), "`n===== $Cmd (exit $code)`n$out")
        return [pscustomobject]@{ Code = $code; Out = $out; Seconds = $sw.Elapsed.TotalSeconds }
    }
    function Latest([string]$Prefix) {
        Get-ChildItem (Join-Path $copy '__dwo-forge\data\runs') -Directory -Filter "$Prefix-*" | Sort-Object Name -Descending | Select-Object -First 1
    }

    $r = Run 'attach'
    Check 'cycle' 'attach' ($r.Code -eq 0 -and $r.Out -notmatch '\[fail\]' -and (Test-Path $bat)) 'doctor all ok, launcher placed' $r.Seconds
    $hidden = (& git -C $copy status --porcelain) -join "`n"
    Check 'cycle' 'hidden from git' ($hidden -eq $before) 'git status unchanged after attach'

    $r = Run 'analyze'
    $run = Latest 'run'
    Check 'cycle' 'analyze' ($r.Code -ne 2 -and (Test-Path "$($run.FullName)\out\report.html")) ($r.Out -split "`n" | Select-String 'verdict:' | Select-Object -Last 1) $r.Seconds

    $r = Run 'verify'
    $v = Latest 'verify'
    $vj = Get-Content -Raw -Encoding UTF8 "$($v.FullName)\out\verify.json" | ConvertFrom-Json
    Check 'cycle' 'verify' ($vj.verdict -ne 'fail') "verdict $($vj.verdict)" $r.Seconds

    $r = Run 'logs'
    Check 'cycle' 'logs' ($r.Code -eq 0 -and (Latest 'logs') -ne $null) '' $r.Seconds

    $r = Run 'sm-install'
    Check 'cycle' 'sm-install' ($r.Code -eq 0 -and (Test-Path "$copy\Plugins\__DwoStateMonitor\DwoStateMonitor.uplugin")) '' $r.Seconds
    $r = Run 'analyze'
    $sm = Get-Content -Raw -Encoding UTF8 "$((Latest 'run').FullName)\out\statemonitor.json" | ConvertFrom-Json
    Check 'cycle' 'analyze sees DwoStateMonitor' ($sm.present -and $sm.testFilter -eq 'StateMonitor.') '' $r.Seconds
    $r = Run 'verify'
    $vj = Get-Content -Raw -Encoding UTF8 "$((Latest 'verify').FullName)\out\verify.json" | ConvertFrom-Json
    $auto = ($vj.steps | Where-Object { $_.id -eq 'V3' }).automation
    $smTests = @($auto.tests | Where-Object { $_.name -like 'StateMonitor.*' })
    $smFailed = @($smTests | Where-Object { $_.state -ne 'Success' })
    Check 'cycle' 'DwoStateMonitor tests' ($smTests.Count -ge 6 -and $smFailed.Count -eq 0) "$($smTests.Count) run, $($smFailed.Count) failed" $r.Seconds

    # DwoCanvas: managed like DwoStateMonitor (hidden install, validator, commandlet import, removal guards).
    $r = Run 'canvas-install'
    $canvasDir = Join-Path $copy 'Plugins\__DwoCanvas'
    $visible = (& git -C $copy status --porcelain --untracked-files=normal -- Plugins/__DwoCanvas) -join "`n"
    Check 'canvas' 'canvas-install' ($r.Code -eq 0 -and (Test-Path "$canvasDir\Binaries\Win64\UnrealEditor-DwoCanvasEditor.dll") -and -not $visible) 'prebuilt, hidden from git' $r.Seconds
    $sample = Join-Path $repo 'plugin\DwoCanvas\Samples\sample_settings.dwocanvas.json'
    $r = Run "canvas-validate `"$sample`""
    Check 'canvas' 'canvas-validate sample' ($r.Code -eq 0) '' $r.Seconds
    $r = Run "canvas-import `"$sample`""
    $reportLine = ($r.Out -split "`n" | Where-Object { $_ -match 'Report: ' } | Select-Object -Last 1)
    Check 'canvas' 'canvas-import sample' ($r.Code -eq 0 -and $reportLine) "$reportLine" $r.Seconds
    $r = Run 'canvas-remove'
    Check 'canvas' 'canvas-remove refuses while widgets use it' ($r.Code -ne 0 -and (Test-Path $canvasDir)) ''
    $r = Run 'uninstall --delete-data'
    Check 'canvas' 'uninstall refuses while widgets use it' ($r.Code -ne 0 -and (Test-Path $canvasDir) -and (Test-Path "$copy\__dwo-forge")) ''
    # Drop what the import generated (widgets, textures, import records) so the rest of the cycle compares clean.
    & git -C $copy clean -fdq -- Content DwoCanvas 2>$null
    $r = Run 'canvas-remove'
    Check 'canvas' 'canvas-remove once no widget uses it' ($r.Code -eq 0 -and -not (Test-Path $canvasDir)) ''
    # A 2.0.0 install (visible Plugins\DwoCanvas with .dwo-forge-installed) moves to Plugins\__DwoCanvas.
    $r = Run 'canvas-install'
    $oldCanvas = Join-Path $copy 'Plugins\DwoCanvas'
    Move-Item $canvasDir $oldCanvas
    Remove-Item -Force (Join-Path $oldCanvas '.dwo-forge-managed')
    Set-Content -Encoding UTF8 (Join-Path $oldCanvas '.dwo-forge-installed') 'DwoCanvas 0.2.0 installed by dwo-Forge'
    $r = Run 'status'
    Check 'canvas' 'status moves a 2.0.0 install' ($r.Code -eq 0 -and -not (Test-Path $oldCanvas) -and (Test-Path "$canvasDir\.dwo-forge-managed")) '' $r.Seconds

    if ($guiExe) {
        $out = Join-Path $qa 'combined-report.html'
        $p = Start-Process -FilePath $guiExe.FullName -ArgumentList "--project `"$copy`" --export-report `"$out`"" -Wait -PassThru
        $html = if (Test-Path $out) { Get-Content -Raw -Encoding UTF8 $out } else { '' }
        Check 'gui' 'export combined report' ($p.ExitCode -eq 0 -and $html -match '아키텍처 분석' -and $html -match '검증' -and $html -match '로그') "$([math]::Round((Get-Item $out -ErrorAction SilentlyContinue).Length / 1KB)) KB"
    }

    # A project that references StateMonitor must not lose it silently.
    $probe = Join-Path $copy 'Source\Probe\Probe.cpp'
    New-Item -ItemType Directory -Force (Split-Path -Parent $probe) | Out-Null
    Set-Content -Encoding UTF8 $probe '#include "StateMonitorSubsystem.h"'
    $r = Run 'sm-remove'
    Check 'safety' 'sm-remove refuses while referenced' ($r.Code -ne 0 -and (Test-Path "$copy\Plugins\__DwoStateMonitor")) ''
    $r = Run 'uninstall --delete-data'
    Check 'safety' 'uninstall refuses while referenced' ($r.Code -ne 0 -and (Test-Path "$copy\__dwo-forge")) ''
    Remove-Item -Recurse -Force (Join-Path $copy 'Source\Probe')
    if (-not (Get-ChildItem (Join-Path $copy 'Source') -ErrorAction SilentlyContinue)) { Remove-Item -Force (Join-Path $copy 'Source') -ErrorAction SilentlyContinue }
    $r = Run 'sm-remove'
    Check 'cycle' 'sm-remove' ($r.Code -eq 0 -and -not (Test-Path "$copy\Plugins\__DwoStateMonitor")) '' $r.Seconds

    # A dwo-Lens 1.x StateMonitor install (editor mode) is migrated by update.
    $legacySm = Join-Path $copy 'Plugins\__StateMonitor'
    New-Item -ItemType Directory -Force $legacySm | Out-Null
    Set-Content -Encoding UTF8 (Join-Path $legacySm 'StateMonitor.uplugin') '{"FileVersion": 3, "Modules": [{"Name": "StateMonitor", "Type": "Runtime", "TargetAllowList": ["Editor"]}]}'
    Set-Content -Encoding UTF8 (Join-Path $legacySm '.dwo-lens-managed') 'dwo-Lens 1.x'
    $r = Run 'update'
    $migrated = Join-Path $copy 'Plugins\__DwoStateMonitor\DwoStateMonitor.uplugin'
    Check 'cycle' 'update' ($r.Code -eq 0 -and (Test-Path $bat) -and -not (Test-Path (Join-Path $copy '__dwo-forge\system'))) 'runs from the install folder' $r.Seconds
    Check 'cycle' 'update migrates __StateMonitor' (-not (Test-Path $legacySm) -and (Test-Path $migrated) -and ((Get-Content -Raw $migrated) -match 'TargetAllowList')) 'same (editor) mode'
    $r = Run 'sm-remove'

    $r = Run 'uninstall --delete-data'
    $leftovers = @(Get-ChildItem -Force $copy | Where-Object { $_.Name -like '__*' }) + @(Get-ChildItem -Force (Join-Path $copy 'Plugins') -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '__*' })
    $after = (@(& git -C $copy status --porcelain) | Where-Object { $_ -notmatch 'Plugins/' }) -join "`n"
    $exclude = Get-Content -Raw (Join-Path $copy '.git\info\exclude')
    Check 'cycle' 'uninstall leaves no trace' ($r.Code -eq 0 -and $leftovers.Count -eq 0 -and $after -eq $before -and $exclude -notmatch 'dwo-(forge|lens) managed') "leftovers $($leftovers.Count)" $r.Seconds
    Check 'canvas' 'uninstall removes DwoCanvas' (-not (Test-Path $canvasDir)) ''
}

# ---------------------------------------------------------------- 5. projects without their own repository
if (-not $SkipIntegration -and $Project) {
    if (Test-Path $Scratch) { Remove-Item -Recurse -Force $Scratch }
    New-Item -ItemType Directory -Force $Scratch | Out-Null
    $env:DWO_FORGE_HOME = $null
    $env:DWO_LENS_HOME = $null
    # An install folder without .git, like an extracted GitHub zip.
    $zip = Join-Path $Scratch 'install'
    & robocopy $repo $zip /E /XD .git .qa Saved /NFL /NDL /NJH /NJS /NP | Out-Null
    $zipLauncher = Join-Path $zip 'dwo-Forge.bat'
    function Copy-Clean([string]$Dest) {
        & robocopy $copy $Dest /E /XD .git Saved Intermediate DerivedDataCache __dwo-forge __dwo-lens /XF __dwo-forge.bat __dwo-lens.bat .gitignore /NFL /NDL /NJH /NJS /NP | Out-Null
    }
    function Leftovers([string]$Dir) {
        @(Get-ChildItem -Force $Dir | Where-Object { $_.Name -like '__*' }) + @(Get-ChildItem -Force (Join-Path $Dir 'Plugins') -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '__*' })
    }

    $plain = Join-Path $Scratch 'nogit\project'
    Copy-Clean $plain
    $sw.Restart()
    $out = & cmd /c "`"$zipLauncher`" attach `"$plain`"" 2>&1 | Out-String
    Check 'novcs' 'attach without git (zip install)' ($LASTEXITCODE -eq 0 -and $out -match 'No version control' -and (Test-Path (Join-Path $plain '__dwo-forge.bat'))) '' $sw.Elapsed.TotalSeconds
    $out = & cmd /c "`"$(Join-Path $plain '__dwo-forge.bat')`" sizes" 2>&1 | Out-String
    Check 'novcs' 'line through the project launcher' ($LASTEXITCODE -eq 0) ''
    $out = & cmd /c "`"$(Join-Path $plain '__dwo-forge.bat')`" uninstall --delete-data" 2>&1 | Out-String
    $left = Leftovers $plain
    Check 'novcs' 'uninstall leaves no trace' ($LASTEXITCODE -eq 0 -and $left.Count -eq 0) "leftovers $($left.Count)"

    $outer = Join-Path $Scratch 'parent'
    $inner = Join-Path $outer 'games\project'
    Copy-Clean $inner
    & git -C $outer init -q
    Set-Content -Encoding ASCII (Join-Path $outer '.gitignore') "games/*/Saved/`ngames/*/Intermediate/`ngames/*/DerivedDataCache/`ngames/*/Binaries/"
    & git -C $outer add -A 2>$null
    & git -C $outer -c user.name=selftest -c user.email=selftest@local commit -q -m baseline
    $before = (& git -C $outer status --porcelain) -join "`n"
    $excl = Join-Path $outer '.git\info\exclude'
    $exclBefore = Get-Content -Raw $excl
    $out = & cmd /c "`"$zipLauncher`" attach `"$inner`"" 2>&1 | Out-String
    $hidden = ((& git -C $outer status --porcelain) -join "`n") -eq $before
    Check 'parent' 'attach inside a parent repository' ($LASTEXITCODE -eq 0 -and $hidden -and (Get-Content -Raw $excl) -match '/games/project/__dwo-forge/') 'prefixed exclude, parent status unchanged'
    $out = & cmd /c "`"$(Join-Path $inner '__dwo-forge.bat')`" uninstall --keep-data" 2>&1 | Out-String
    $kept = if ($out -match 'kept at (.+)') { $Matches[1].Trim() } else { '' }
    $left = Leftovers $inner
    $clean = ((& git -C $outer status --porcelain) -join "`n") -eq $before -and (Get-Content -Raw $excl) -eq $exclBefore
    Check 'parent' 'uninstall restores the parent exclude' ($LASTEXITCODE -eq 0 -and $left.Count -eq 0 -and $clean) "leftovers $($left.Count)"
    Check 'parent' 'kept history moved out of the project' ($kept -and (Test-Path $kept)) $kept
    if ($kept -and (Test-Path $kept)) { Remove-Item -Recurse -Force $kept }
    Remove-Item -Recurse -Force $Scratch -ErrorAction SilentlyContinue
}

$failed = @($checks | Where-Object { -not $_.ok })
$summary = [ordered]@{ ok = ($failed.Count -eq 0); total = $checks.Count; failed = $failed.Count; system = (& git -C $repo rev-parse --short HEAD); checks = $checks }
Write-Output ($summary | ConvertTo-Json -Depth 5) | Set-Content -Encoding UTF8 (Join-Path $qa 'selftest-result.json')
Write-Host "[selftest] $($checks.Count - $failed.Count)/$($checks.Count) passed"
if ($failed.Count -gt 0) { exit 1 }

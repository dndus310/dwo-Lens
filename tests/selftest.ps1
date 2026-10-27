# dwo-Assay self verification. Runs everything a release must pass and writes .qa\selftest\selftest-result.json.
#
#   powershell -File tests\selftest.ps1 -Project <a UE project folder to copy for the integration cycle>
#
# The given project is never modified: the integration cycle runs on an isolated copy under .qa\.
param(
    [string]$Project = '',
    [switch]$SkipIntegration
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
foreach ($bat in (Get-ChildItem $repo -Filter *.bat)) {
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
$guiExe = Get-ChildItem (Join-Path $env:LOCALAPPDATA 'dwo-Assay\gui-cache') -Recurse -Filter dwo-assay-gui.exe -ErrorAction SilentlyContinue |
          Sort-Object LastWriteTime -Descending | Select-Object -First 1
Check 'gui' 'build' ($LASTEXITCODE -eq 0 -and $guiExe -ne $null) "$($guiExe.FullName)" $sw.Elapsed.TotalSeconds

# ---------------------------------------------------------------- 4. integration cycle on an isolated copy
if (-not $SkipIntegration -and $Project) {
    $copy = Join-Path $qa 'project'
    New-Item -ItemType Directory -Force $copy | Out-Null
    Get-ChildItem $Project | Where-Object { $_.Name -notin @('Intermediate', 'Saved', 'DerivedDataCache', '.git', '.vs', '__dwo-assay', '__dwo-assay.bat') -and $_.Name -notlike '*.html' -and $_.Extension -ne '.sln' } |
        ForEach-Object { Copy-Item -Recurse $_.FullName $copy }
    Get-ChildItem -Recurse -Directory -Path (Join-Path $copy 'Plugins') -Filter '__*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
    & git -C $copy init -q
    # Standard UE ignores so editor work folders do not count as project changes.
    Set-Content -Encoding ASCII (Join-Path $copy '.gitignore') "Saved/`nIntermediate/`nDerivedDataCache/`nBinaries/`n.vs/"
    & git -C $copy add -A
    & git -C $copy -c user.name=selftest -c user.email=selftest@local commit -q -m baseline
    $before = (& git -C $copy status --porcelain) -join "`n"
    $env:DWO_ASSAY_HOME = $repo
    Copy-Item (Join-Path $repo '__dwo-assay.bat') $copy
    $bat = Join-Path $copy '__dwo-assay.bat'
    function Run([string]$Cmd) {
        $sw.Restart()
        $out = & cmd /c "`"$bat`" $Cmd" 2>&1 | Out-String
        $code = $LASTEXITCODE
        [IO.File]::AppendAllText((Join-Path $qa 'integration.log'), "`n===== $Cmd (exit $code)`n$out")
        return [pscustomobject]@{ Code = $code; Out = $out; Seconds = $sw.Elapsed.TotalSeconds }
    }
    function Latest([string]$Prefix) {
        Get-ChildItem (Join-Path $copy '__dwo-assay\data\runs') -Directory -Filter "$Prefix-*" | Sort-Object Name -Descending | Select-Object -First 1
    }

    $r = Run 'attach'
    Check 'cycle' 'attach' ($r.Code -eq 0 -and $r.Out -notmatch '\[fail\]') 'doctor all ok' $r.Seconds
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
    Check 'cycle' 'sm-install' ($r.Code -eq 0 -and (Test-Path "$copy\Plugins\__StateMonitor\StateMonitor.uplugin")) '' $r.Seconds
    $r = Run 'analyze'
    $sm = Get-Content -Raw -Encoding UTF8 "$((Latest 'run').FullName)\out\statemonitor.json" | ConvertFrom-Json
    Check 'cycle' 'analyze sees StateMonitor' ($sm.present -and $sm.testFilter -eq 'StateMonitor.') '' $r.Seconds
    $r = Run 'verify'
    $vj = Get-Content -Raw -Encoding UTF8 "$((Latest 'verify').FullName)\out\verify.json" | ConvertFrom-Json
    $auto = ($vj.steps | Where-Object { $_.id -eq 'V3' }).automation
    $smTests = @($auto.tests | Where-Object { $_.name -like 'StateMonitor.*' })
    $smFailed = @($smTests | Where-Object { $_.state -ne 'Success' })
    Check 'cycle' 'StateMonitor tests' ($smTests.Count -ge 6 -and $smFailed.Count -eq 0) "$($smTests.Count) run, $($smFailed.Count) failed" $r.Seconds

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
    Check 'safety' 'sm-remove refuses while referenced' ($r.Code -ne 0 -and (Test-Path "$copy\Plugins\__StateMonitor")) ''
    $r = Run 'detach'
    Check 'safety' 'detach refuses while referenced' ($r.Code -ne 0 -and (Test-Path "$copy\__dwo-assay")) ''
    Remove-Item -Recurse -Force (Join-Path $copy 'Source\Probe')
    if (-not (Get-ChildItem (Join-Path $copy 'Source') -ErrorAction SilentlyContinue)) { Remove-Item -Force (Join-Path $copy 'Source') -ErrorAction SilentlyContinue }
    $r = Run 'sm-remove'
    Check 'cycle' 'sm-remove' ($r.Code -eq 0 -and -not (Test-Path "$copy\Plugins\__StateMonitor")) '' $r.Seconds

    $r = Run 'update'
    $dirty = (& git -C (Join-Path $copy '__dwo-assay\system') status --porcelain) -join ' '
    Check 'cycle' 'update keeps clone clean' ($r.Code -eq 0 -and -not $dirty) "$dirty" $r.Seconds

    $r = Run 'detach'
    $leftovers = @(Get-ChildItem -Force $copy | Where-Object { $_.Name -like '__*' }) + @(Get-ChildItem -Force (Join-Path $copy 'Plugins') -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '__*' })
    $after = (& git -C $copy status --porcelain) -join "`n"
    $exclude = Get-Content -Raw (Join-Path $copy '.git\info\exclude')
    Check 'cycle' 'detach leaves no trace' ($r.Code -eq 0 -and $leftovers.Count -eq 0 -and $after -eq $before -and $exclude -notmatch 'dwo-assay managed') "leftovers $($leftovers.Count)" $r.Seconds
}

$failed = @($checks | Where-Object { -not $_.ok })
$summary = [ordered]@{ ok = ($failed.Count -eq 0); total = $checks.Count; failed = $failed.Count; system = (& git -C $repo rev-parse --short HEAD); checks = $checks }
Write-Output ($summary | ConvertTo-Json -Depth 5) | Set-Content -Encoding UTF8 (Join-Path $qa 'selftest-result.json')
Write-Host "[selftest] $($checks.Count - $failed.Count)/$($checks.Count) passed"
if ($failed.Count -gt 0) { exit 1 }

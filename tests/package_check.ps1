# Packaging check: copies a UE project into an isolated QA folder, attaches dwo-Lens (optionally installs
# StateMonitor), packages it with BuildCookRun, smoke-runs the packaged game, and verifies that nothing from
# the editor-only inspector reaches the package. Writes <QaRoot>\<Name>\package-result.json.
#
#   powershell -File tests\package_check.ps1 -Source <project folder> -Name pkg_bp -Config Shipping [-InstallStateMonitor]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Name,
    [ValidateSet('Development', 'Shipping')][string]$Config = 'Shipping',
    [string]$QaRoot = '',
    [switch]$InstallStateMonitor,
    [int]$SmokeSeconds = 20
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $QaRoot) { $QaRoot = Join-Path $repo '.qa' }
$work = Join-Path $QaRoot $Name
$project = Join-Path $work 'project'
$archive = Join-Path $work "package-$Config"
$result = [ordered]@{ name = $Name; config = $Config; source = $Source; steps = @() }
function Step([string]$Id, [bool]$Ok, [string]$Detail, [double]$Seconds = 0) {
    $script:result.steps += [ordered]@{ id = $Id; ok = $Ok; detail = $Detail; seconds = [math]::Round($Seconds, 1) }
    $mark = if ($Ok) { 'ok  ' } else { 'FAIL' }
    Write-Host "[qa] [$mark] $Id - $Detail"
}
function Save { Write-Output ($result | ConvertTo-Json -Depth 6) | Set-Content -Encoding UTF8 (Join-Path $work 'package-result.json') }

# ---- isolated copy
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force $project | Out-Null
Get-ChildItem $Source | Where-Object { $_.Name -notin @('Intermediate', 'Saved', 'DerivedDataCache', '.git', '.vs', '__dwo-lens', '__dwo-lens.bat') -and $_.Extension -ne '.sln' -and $_.Name -notlike '*.html' } |
    ForEach-Object { Copy-Item -Recurse $_.FullName $project }
Get-ChildItem -Recurse -Directory -Path (Join-Path $project 'Plugins') -Filter '__*' -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force
& git -C $project init -q
$uproject = (Get-ChildItem $project -Filter *.uproject | Select-Object -First 1).FullName
Step 'copy' $true "isolated copy at $project"

# ---- attach (+ StateMonitor)
$env:DWO_LENS_HOME = $repo
Copy-Item (Join-Path $repo '__dwo-lens.bat') $project
$sw = [Diagnostics.Stopwatch]::StartNew()
& cmd /c "`"$project\__dwo-lens.bat`" attach" | Out-Null
Step 'attach' ($LASTEXITCODE -eq 0 -and (Test-Path "$project\Plugins\__DwoLensInspector")) 'dwo-Lens attached' $sw.Elapsed.TotalSeconds
if ($InstallStateMonitor) {
    $sw.Restart()
    & cmd /c "`"$project\__dwo-lens.bat`" sm-install" | Out-Null
    Step 'sm-install' ($LASTEXITCODE -eq 0 -and (Test-Path "$project\Plugins\__StateMonitor")) 'StateMonitor installed' $sw.Elapsed.TotalSeconds
}

# ---- package
$engineRoot = (Get-Content -Raw "$project\__dwo-lens\state.json" | ConvertFrom-Json).engineRoot
$uat = Join-Path $engineRoot 'Engine\Build\BatchFiles\RunUAT.bat'
$log = Join-Path $work "buildcookrun-$Config.log"
$sw.Restart()
& cmd /c "`"$uat`" BuildCookRun -project=`"$uproject`" -noP4 -platform=Win64 -clientconfig=$Config -build -cook -stage -pak -archive -archivedirectory=`"$archive`" -unattended -utf8output -nocompileeditor > `"$log`" 2>&1"
$pkgCode = $LASTEXITCODE
# The project launcher (<Project>.exe at the package root) passes the project to the engine binary;
# starting UnrealGame-*.exe directly exits at once because it does not know which project to run.
$projectName = [IO.Path]::GetFileNameWithoutExtension($uproject)
$exe = Get-ChildItem -Recurse -Path $archive -Filter "$projectName.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
Step 'package' ($pkgCode -eq 0 -and $exe -ne $null) "BuildCookRun $Config exit $pkgCode, exe: $(if ($exe) { $exe.FullName.Substring($archive.Length + 1) } else { 'none' })" $sw.Elapsed.TotalSeconds
if (-not $exe) { Save; exit 1 }

# ---- inspect package: the editor-only inspector must not be in the package
$pkgFiles = Get-ChildItem -Recurse -File $archive
$inspectorFiles = @($pkgFiles | Where-Object { $_.FullName -match 'DwoLens' })
Step 'no-inspector-files' ($inspectorFiles.Count -eq 0) "files named DwoLens in package: $($inspectorFiles.Count)"
$binaryHits = 0
foreach ($f in ($pkgFiles | Where-Object { $_.Extension -in @('.exe', '.dll') })) {
    if ([IO.File]::ReadAllText($f.FullName, [Text.Encoding]::ASCII).Contains('DwoLensInspector')) { $binaryHits++ }
}
Step 'no-inspector-symbols' ($binaryHits -eq 0) "binaries containing 'DwoLensInspector': $binaryHits"
$unrealPak = Join-Path $engineRoot 'Engine\Binaries\Win64\UnrealPak.exe'
$paks = @($pkgFiles | Where-Object { $_.Extension -eq '.pak' })
$pakHits = 0; $smInPak = 0
foreach ($pak in $paks) {
    $listing = & $unrealPak $pak.FullName -List 2>$null
    $pakHits += @($listing | Select-String -SimpleMatch 'DwoLens').Count
    $smInPak += @($listing | Select-String -SimpleMatch 'StateMonitor').Count
}
Step 'no-inspector-in-pak' ($pakHits -eq 0) "pak entries mentioning DwoLens: $pakHits (paks: $($paks.Count))"
$smBinary = 0
foreach ($f in ($pkgFiles | Where-Object { $_.Extension -in @('.exe', '.dll') })) {
    if ([IO.File]::ReadAllText($f.FullName, [Text.Encoding]::ASCII).Contains('StateMonitorSubsystem')) { $smBinary++ }
}
$result.stateMonitorInPackage = [ordered]@{ binaries = $smBinary; pakEntries = $smInPak }
$result.packageSizeMB = [math]::Round((($pkgFiles | Measure-Object Length -Sum).Sum) / 1MB, 1)

# ---- smoke run
$sw.Restart()
$p = Start-Process -FilePath $exe.FullName -ArgumentList '-unattended -nullrhi -nosound -windowed -ResX=640 -ResY=360' -PassThru
Start-Sleep -Seconds $SmokeSeconds
$alive = -not $p.HasExited
$exitCode = if ($p.HasExited) { $p.ExitCode } else { $null }
if ($alive) { Stop-Process -Id $p.Id -Force }
Get-Process | Where-Object { $_.Path -and $_.Path.StartsWith($archive) } | Stop-Process -Force -ErrorAction SilentlyContinue
$crashDirs = @(Get-ChildItem -Recurse -Directory -Path $archive -Filter 'Crashes' -ErrorAction SilentlyContinue | Get-ChildItem -Directory)
Step 'smoke-run' ($alive -and $crashDirs.Count -eq 0) "ran $SmokeSeconds s: alive=$alive exit=$exitCode crash folders=$($crashDirs.Count)" $sw.Elapsed.TotalSeconds

# ---- detach leaves the copy clean
& cmd /c "`"$project\__dwo-lens.bat`" detach --force" | Out-Null
$left = @(Get-ChildItem -Force $project | Where-Object { $_.Name -like '__*' }) + @(Get-ChildItem -Force (Join-Path $project 'Plugins') -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '__*' })
Step 'detach' ($left.Count -eq 0) "leftover dwo-Lens items after detach: $($left.Count)"

$result.ok = -not ($result.steps | Where-Object { -not $_.ok })
Save
if (-not $result.ok) { exit 1 }

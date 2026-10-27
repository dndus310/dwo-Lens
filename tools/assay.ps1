# dwo-Assay control script. Invoked by __dwo-assay.bat from the project root.
# Windows PowerShell 5.1 compatible. Never modifies tracked project files.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('attach', 'update', 'status', 'analyze', 'doctor', 'detach')]
    [string]$Command,
    [Parameter(Mandatory = $true)]
    [string]$Project,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = 'Stop'
$Utf8 = New-Object System.Text.UTF8Encoding($false)

$SystemDir   = Split-Path -Parent $PSScriptRoot
$Project     = (Resolve-Path $Project).Path.TrimEnd('\')
$AssayDir    = Join-Path $Project '__dwo-assay'
$DataDir     = Join-Path $AssayDir 'data'
$StatePath   = Join-Path $AssayDir 'state.json'
$PluginDest  = Join-Path $Project 'Plugins\__DwoAssayInspector'
$Marker      = '.dwo-assay-managed'
$BlockStart  = '# >>> dwo-assay managed (do not edit) >>>'
$BlockEnd    = '# <<< dwo-assay managed <<<'
$ManagedPaths = @('/__dwo-assay/', '/__dwo-assay.bat', '/Plugins/__DwoAssayInspector/')

function Say([string]$Message) { Write-Host "[dwo-assay] $Message" }
function Fail([string]$Message) { Write-Host "[dwo-assay] ERROR: $Message" -ForegroundColor Red; exit 1 }

function Write-Json($Path, $Object) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [IO.File]::WriteAllText($Path, ($Object | ConvertTo-Json -Depth 10), $Utf8)
}

function Read-State {
    if (Test-Path $StatePath) { return Get-Content -Raw -Encoding UTF8 $StatePath | ConvertFrom-Json }
    return $null
}

# ---------------------------------------------------------------- project / engine
function Get-UProject {
    $files = @(Get-ChildItem -Path $Project -Filter *.uproject -File)
    if ($files.Count -ne 1) { Fail "Expected exactly one .uproject in $Project, found $($files.Count)." }
    $json = Get-Content -Raw -Encoding UTF8 $files[0].FullName | ConvertFrom-Json
    return [pscustomobject]@{ Path = $files[0].FullName; Name = $files[0].BaseName; Json = $json;
                              IsCode = (Test-Path (Join-Path $Project 'Source')) }
}

function Resolve-Engine([string]$Association) {
    $root = $null
    if ($Association -match '^\{.+\}$') {
        $key = 'HKCU:\Software\Epic Games\Unreal Engine\Builds'
        if (Test-Path $key) { $root = (Get-ItemProperty $key).$Association }
    } else {
        $dat = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
        if (Test-Path $dat) {
            $entry = (Get-Content -Raw $dat | ConvertFrom-Json).InstallationList |
                     Where-Object { $_.AppName -eq "UE_$Association" } | Select-Object -First 1
            if ($entry) { $root = $entry.InstallLocation }
        }
        if (-not $root) {
            $key = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$Association"
            if (Test-Path $key) { $root = (Get-ItemProperty $key).InstalledDirectory }
        }
    }
    if (-not $root -or -not (Test-Path $root)) { Fail "Cannot find the engine for EngineAssociation '$Association'." }
    $root = (Resolve-Path $root).Path
    $versionFile = Join-Path $root 'Engine\Build\Build.version'
    $v = Get-Content -Raw $versionFile | ConvertFrom-Json
    return [pscustomobject]@{
        Root    = $root
        Version = "$($v.MajorVersion).$($v.MinorVersion).$($v.PatchVersion)"
        Cmd     = Join-Path $root 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
        RunUAT  = Join-Path $root 'Engine\Build\BatchFiles\RunUAT.bat'
    }
}

# ---------------------------------------------------------------- git
function Invoke-Git([string[]]$GitArgs) {
    $out = & git -C $Project @GitArgs 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return $out
}

function Assert-OwnRepository {
    $top = Invoke-Git @('rev-parse', '--show-toplevel')
    if (-not $top) { Fail "$Project is not inside a git repository." }
    $topNorm = ($top -replace '/', '\').TrimEnd('\')
    if ($topNorm -ne $Project) {
        Fail "The project is not its own git repository. Enclosing repository: $topNorm. Run 'git init' in the project or use its real clone."
    }
}

function Get-ExcludePath {
    $p = Invoke-Git @('rev-parse', '--path-format=absolute', '--git-path', 'info/exclude')
    if (-not $p) { Fail 'Cannot resolve .git/info/exclude.' }
    return ($p -replace '/', '\')
}

function Get-ExcludeLines([string]$Path) {
    if (-not (Test-Path $Path)) { return @() }
    return @([IO.File]::ReadAllLines($Path))
}

function Set-ExcludeBlock {
    $path = Get-ExcludePath
    $lines = Get-ExcludeLines $path
    $kept = Remove-Block $lines
    $block = @($BlockStart) + $ManagedPaths + @($BlockEnd)
    $all = @($kept | Where-Object { $_ -ne $null }) + @('') + $block
    $dir = Split-Path -Parent $path
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [IO.File]::WriteAllText($path, (($all -join "`n").Trim() + "`n"), $Utf8)
}

function Remove-Block([string[]]$Lines) {
    $out = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in $Lines) {
        if ($line -like '# >>> dwo-assay managed*') { $inside = $true; continue }
        if ($line -like '# <<< dwo-assay managed*') { $inside = $false; continue }
        if (-not $inside) { $out.Add($line) }
    }
    while ($out.Count -gt 0 -and [string]::IsNullOrWhiteSpace($out[$out.Count - 1])) { $out.RemoveAt($out.Count - 1) }
    return ,$out.ToArray()
}

function Clear-ExcludeBlock {
    $path = Get-ExcludePath
    if (-not (Test-Path $path)) { return }
    $kept = Remove-Block (Get-ExcludeLines $path)
    $text = ''
    if ($kept.Count -gt 0) { $text = ($kept -join "`n") + "`n" }
    [IO.File]::WriteAllText($path, $text, $Utf8)
}

# Folders the engine itself rewrites whenever an editor or commandlet runs (ignored by the standard UE .gitignore).
$VolatileDirs = @('Saved/', 'Intermediate/', 'DerivedDataCache/', 'Binaries/', '.vs/')

function Get-ProjectStatus {
    # Porcelain status of the project, used to prove attach/detach leave no trace.
    # Engine work folders are excluded: running the editor always touches them.
    $s = Invoke-Git @('status', '--porcelain', '--untracked-files=all')
    if (-not $s) { return '' }
    $kept = @($s) | Where-Object {
        $path = $_.Substring(3).Trim('"')
        -not ($VolatileDirs | Where-Object { $path.StartsWith($_) })
    }
    return (@($kept) -join "`n")
}

# ---------------------------------------------------------------- plugin
function Get-PluginSourceHash {
    $src = Join-Path $SystemDir 'plugin\DwoAssayInspector'
    $sha = [Security.Cryptography.SHA256]::Create()
    $sb = New-Object System.Text.StringBuilder
    Get-ChildItem -Recurse -File $src | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($src.Length)
        $h = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
        [void]$sb.Append("$rel=$h;")
    }
    $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($sb.ToString()))
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 12)
}

function Get-PluginBuild($Engine) {
    $hash = Get-PluginSourceHash
    $cacheRoot = Join-Path $env:LOCALAPPDATA 'dwo-Assay\plugin-cache'
    $cache = Join-Path $cacheRoot "$($Engine.Version)-$hash"
    $dll = Join-Path $cache 'Binaries\Win64\UnrealEditor-DwoAssayInspector.dll'
    if (Test-Path $dll) { return [pscustomobject]@{ Dir = $cache; Hash = $hash } }

    Say "Building DwoAssayInspector for UE $($Engine.Version) (first time for this engine/plugin version, takes a few minutes)..."
    $staging = Join-Path $cacheRoot "staging-$hash"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    $log = Join-Path $DataDir 'logs\build-plugin.log'
    New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null
    $uplugin = Join-Path $SystemDir 'plugin\DwoAssayInspector\DwoAssayInspector.uplugin'
    & cmd /c "`"$($Engine.RunUAT)`" BuildPlugin -Plugin=`"$uplugin`" -Package=`"$staging`" -HostPlatforms=Win64 -TargetPlatforms=Win64 > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $staging 'Binaries\Win64\UnrealEditor-DwoAssayInspector.dll'))) {
        Fail "Plugin build failed. See $log"
    }
    if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
    Move-Item $staging $cache
    return [pscustomobject]@{ Dir = $cache; Hash = $hash }
}

function Install-Plugin($Build) {
    Remove-Plugin
    New-Item -ItemType Directory -Force $PluginDest | Out-Null
    # Prebuilt binaries only: the project's own build never compiles this plugin.
    Copy-Item (Join-Path $Build.Dir 'DwoAssayInspector.uplugin') $PluginDest
    # BuildPlugin drops EnabledByDefault; restore it so the project never needs a .uproject entry.
    $upluginPath = Join-Path $PluginDest 'DwoAssayInspector.uplugin'
    $uplugin = Get-Content -Raw -Encoding UTF8 $upluginPath | ConvertFrom-Json
    $uplugin | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $true -Force
    [IO.File]::WriteAllText($upluginPath, ($uplugin | ConvertTo-Json -Depth 10), $Utf8)
    Copy-Item -Recurse (Join-Path $Build.Dir 'Binaries') $PluginDest
    if (Test-Path (Join-Path $Build.Dir 'Resources')) { Copy-Item -Recurse (Join-Path $Build.Dir 'Resources') $PluginDest }
    [IO.File]::WriteAllText((Join-Path $PluginDest $Marker), "Managed by dwo-Assay. Removed by '__dwo-assay.bat detach'.`n", $Utf8)
}

function Remove-Plugin {
    if (-not (Test-Path $PluginDest)) { return }
    if (-not (Test-Path (Join-Path $PluginDest $Marker))) {
        Fail "$PluginDest exists but is not managed by dwo-Assay. Leaving it untouched."
    }
    Remove-Item -Recurse -Force $PluginDest
    $plugins = Join-Path $Project 'Plugins'
    if ((Test-Path $plugins) -and -not (Get-ChildItem -Force $plugins)) { Remove-Item -Force $plugins }
}

# ---------------------------------------------------------------- commands
function Get-SystemCommit {
    $c = & git -C $SystemDir rev-parse --short HEAD 2>$null
    if ($LASTEXITCODE -ne 0) { return 'unknown' }
    return $c
}

function Invoke-Attach([string]$Mode) {
    Assert-OwnRepository
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $previous = Read-State
    $baseline = $null
    if ($previous -and $previous.gitStatusBeforeAttach -ne $null) { $baseline = $previous.gitStatusBeforeAttach }

    Set-ExcludeBlock
    if ($baseline -eq $null) { $baseline = Get-ProjectStatus }

    $build = Get-PluginBuild $engine
    Install-Plugin $build

    $state = [ordered]@{
        schema                = 'assay.state/0.1'
        project               = $uproject.Name
        projectType           = $(if ($uproject.IsCode) { 'code' } else { 'blueprint-only' })
        engineVersion         = $engine.Version
        engineRoot            = $engine.Root
        systemCommit          = Get-SystemCommit
        pluginHash            = $build.Hash
        attachedAt            = $(if ($previous) { $previous.attachedAt } else { (Get-Date).ToString('o') })
        updatedAt             = (Get-Date).ToString('o')
        gitStatusBeforeAttach = $baseline
    }
    Write-Json $StatePath $state
    Say "$Mode complete: $($uproject.Name) / UE $($engine.Version) / system $($state.systemCommit)"
    Invoke-Doctor | Out-Null
}

function Invoke-Status {
    $state = Read-State
    if (-not $state) { Say 'Not attached.'; return }
    Say "Project        : $($state.project) ($($state.projectType))"
    Say "Engine         : UE $($state.engineVersion)"
    Say "System commit  : $($state.systemCommit)"
    if ($env:DWO_ASSAY_HOME -and (Test-Path (Join-Path $env:DWO_ASSAY_HOME '.git'))) {
        $latest = & git -C $env:DWO_ASSAY_HOME rev-parse --short HEAD 2>$null
        if ($latest -and $latest -ne $state.systemCommit) { Say "Update available: $latest (run __dwo-assay.bat update)" }
        else { Say 'System is up to date.' }
    }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    if ($engine.Version -ne $state.engineVersion) { Say "Engine changed to $($engine.Version). Run __dwo-assay.bat update." }
    Say "Attached at    : $($state.attachedAt)"
}

function Invoke-Doctor {
    $ok = $true
    function Check([string]$Name, [bool]$Pass, [string]$Hint) {
        if ($Pass) { Write-Host "  [ok]   $Name" } else { Write-Host "  [fail] $Name - $Hint" -ForegroundColor Yellow; $script:doctorFailed = $true }
    }
    $script:doctorFailed = $false
    Say 'doctor'
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    Check "engine UE $($engine.Version)" (Test-Path $engine.Cmd) 'UnrealEditor-Cmd.exe not found'
    $top = Invoke-Git @('rev-parse', '--show-toplevel')
    Check 'project is its own git repository' ($top -and (($top -replace '/', '\').TrimEnd('\') -eq $Project)) 'run git init in the project'
    $excl = Get-ExcludePath
    Check 'exclude block present' ((Test-Path $excl) -and ((Get-Content -Raw $excl) -like "*$BlockStart*")) 'run __dwo-assay.bat attach'
    Check 'plugin installed' (Test-Path (Join-Path $PluginDest 'Binaries\Win64\UnrealEditor-DwoAssayInspector.dll')) 'run __dwo-assay.bat attach'
    $leaks = Invoke-Git @('status', '--porcelain', '--untracked-files=all', '--', '__dwo-assay', '__dwo-assay.bat', 'Plugins/__DwoAssayInspector')
    Check 'dwo-assay files hidden from git' (-not $leaks) "visible to git: $leaks"
    $py = Get-Command python -ErrorAction SilentlyContinue
    Check 'python available' ($py -ne $null) 'install Python 3.10+'
    if ($uproject.IsCode) {
        Write-Host '  [note] code project: confirm your own builds ignore Plugins/__DwoAssayInspector (prebuilt, no Source).'
    }
    return (-not $script:doctorFailed)
}

function Invoke-Analyze {
    $state = Read-State
    if (-not $state) { Fail 'Not attached. Run __dwo-assay.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $runId = 'run-' + (Get-Date).ToString('yyyyMMdd-HHmmss')
    $runDir = Join-Path $DataDir "runs\$runId"
    $rawDir = Join-Path $runDir 'raw'
    $outDir = Join-Path $runDir 'out'
    New-Item -ItemType Directory -Force $rawDir, $outDir | Out-Null
    $stations = New-Object System.Collections.Generic.List[object]

    Say "[$runId] ST-1 collect: running DwoAssayInspector commandlet"
    $log = Join-Path $runDir 'commandlet.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & cmd /c "`"$($engine.Cmd)`" `"$($uproject.Path)`" -run=DwoAssayInspector -AssayOut=`"$rawDir`" -ABSLOG=`"$(Join-Path $runDir 'editor.log')`" -unattended -nullrhi -nosplash -nopause -NoSound -stdout -FullStdOutLogOutput > `"$log`" 2>&1"
    $code = $LASTEXITCODE
    $sw.Stop()
    $collected = Test-Path (Join-Path $rawDir 'meta.json')
    $stations.Add([ordered]@{ id = 'ST-1'; name = 'collect'; verdict = $(if ($collected) { 'pass' } else { 'fail' }); exitCode = $code; ms = $sw.ElapsedMilliseconds; log = 'commandlet.log' })
    if (-not $collected) { Write-Json (Join-Path $runDir 'run.json') @{ runId = $runId; stations = $stations; final = 'fail' }; Fail "Commandlet produced no output. See $log" }

    Say "[$runId] ST-2/3 analyze and gate"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & python (Join-Path $SystemDir 'tools\assay_analyze.py') --raw $rawDir --project $uproject.Path --out $outDir --run-id $runId
    $pyCode = $LASTEXITCODE
    $sw.Stop()
    $findingsPath = Join-Path $outDir 'findings.json'
    $verdict = 'fail'
    if ($pyCode -eq 0 -and (Test-Path $findingsPath)) { $verdict = (Get-Content -Raw -Encoding UTF8 $findingsPath | ConvertFrom-Json).verdict }
    $stations.Add([ordered]@{ id = 'ST-2'; name = 'analyze+gate'; verdict = $verdict; exitCode = $pyCode; ms = $sw.ElapsedMilliseconds })

    $run = [ordered]@{
        schema = 'assay.run/0.1'; runId = $runId; line = 'architecture-report'
        project = $state.project; engineVersion = $engine.Version; systemCommit = Get-SystemCommit
        startedAt = (Get-Date).ToString('o'); stations = $stations; final = $verdict
    }
    Write-Json (Join-Path $runDir 'run.json') $run
    Say "[$runId] verdict: $verdict"
    Say "Report: $(Join-Path $outDir 'report.html')"
    if ($verdict -eq 'fail') { exit 2 }
}

function Invoke-Detach {
    $keepData = $Rest -contains '--keep-data'
    $state = Read-State
    Remove-Plugin
    Clear-ExcludeBlock
    if ($keepData -and (Test-Path $DataDir)) {
        $dest = Join-Path $env:LOCALAPPDATA ("dwo-Assay\detached\" + (Split-Path -Leaf $Project) + '-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        Move-Item $DataDir $dest
        Say "Analysis data kept at $dest"
    }
    Set-Location $Project
    if (Test-Path $AssayDir) { Remove-Item -Recurse -Force $AssayDir }

    # __dwo-assay.bat deletes itself after this script returns; exclude it from the comparison.
    $after = (@((Get-ProjectStatus) -split "`n") | Where-Object { $_ -and $_ -notmatch '__dwo-assay\.bat$' }) -join "`n"
    if ($state -and $state.gitStatusBeforeAttach -ne $null) {
        if ($after -eq $state.gitStatusBeforeAttach) { Say 'Detached. git status matches the state before attach.' }
        else { Say 'Detached, but git status differs from before attach. Review: git status'; exit 3 }
    } else {
        Say 'Detached.'
    }
}

switch ($Command) {
    'attach'  { Invoke-Attach 'attach' }
    'update'  { Invoke-Attach 'update' }
    'status'  { Invoke-Status }
    'doctor'  { if (-not (Invoke-Doctor)) { exit 1 } }
    'analyze' { Invoke-Analyze }
    'detach'  { Invoke-Detach }
}

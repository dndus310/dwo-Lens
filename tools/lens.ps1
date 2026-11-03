# dwo-Lens control script. Invoked by __dwo-lens.bat from the project root.
# Windows PowerShell 5.1 compatible. Never modifies tracked project files.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('attach', 'update', 'status', 'analyze', 'verify', 'logs', 'tools', 'gui', 'sm-install', 'sm-remove', 'doctor', 'detach')]
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
$LensDir    = Join-Path $Project '__dwo-lens'
$DataDir     = Join-Path $LensDir 'data'
$StatePath   = Join-Path $LensDir 'state.json'
$PluginDest  = Join-Path $Project 'Plugins\__DwoLensInspector'
$Marker      = '.dwo-lens-managed'
$BlockStart  = '# >>> dwo-lens managed (do not edit) >>>'
$BlockEnd    = '# <<< dwo-lens managed <<<'
$SmDest      = Join-Path $Project 'Plugins\__StateMonitor'
$ManagedPaths = @('/__dwo-lens/', '/__dwo-lens.bat', '/Plugins/__DwoLensInspector/', '/Plugins/__StateMonitor/')

function Say([string]$Message) { Write-Host "[dwo-lens] $Message" }
function Fail([string]$Message) { Write-Host "[dwo-lens] ERROR: $Message" -ForegroundColor Red; exit 1 }

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

function Test-OwnRepository {
    # An empty prefix means the project folder is the repository root. Comparing prefixes instead of paths
    # stays correct when the same folder is reached through a junction, symlink or redirected profile path.
    $prefix = & git -C $Project rev-parse --show-prefix 2>$null
    return ($LASTEXITCODE -eq 0 -and [string]::IsNullOrEmpty(($prefix | Out-String).Trim()))
}

function Assert-OwnRepository {
    $top = Invoke-Git @('rev-parse', '--show-toplevel')
    if (-not $top) { Fail "$Project is not inside a git repository." }
    if (-not (Test-OwnRepository)) {
        Fail "The project is not its own git repository. Enclosing repository: $top. Run 'git init' in the project or use its real clone."
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
        if ($line -like '# >>> dwo-lens managed*') { $inside = $true; continue }
        if ($line -like '# <<< dwo-lens managed*') { $inside = $false; continue }
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
function Get-PluginSourceHash([string]$Name = 'DwoLensInspector') {
    $src = Join-Path $SystemDir "plugin\$Name"
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

function Get-PluginBuild($Engine, [string]$Name = 'DwoLensInspector') {
    $hash = Get-PluginSourceHash $Name
    $cacheRoot = Join-Path $env:LOCALAPPDATA 'dwo-Lens\plugin-cache'
    $cache = Join-Path $cacheRoot "$($Engine.Version)-$Name-$hash"
    $dll = Join-Path $cache "Binaries\Win64\UnrealEditor-$Name.dll"
    if (Test-Path $dll) { return [pscustomobject]@{ Dir = $cache; Hash = $hash } }

    Say "Building $Name for UE $($Engine.Version) (first time for this engine/plugin version, takes a few minutes)..."
    $staging = Join-Path $cacheRoot "staging-$Name-$hash"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    $log = Join-Path $DataDir "logs\build-$Name.log"
    New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null
    # Build from a copy: BuildPlugin writes files (Config/FilterPlugin.ini) into the plugin folder,
    # and the system clone must stay clean so 'update' (git pull) keeps working.
    $srcCopy = Join-Path $cacheRoot "src-$Name-$hash"
    if (Test-Path $srcCopy) { Remove-Item -Recurse -Force $srcCopy }
    Copy-Item -Recurse (Join-Path $SystemDir "plugin\$Name") $srcCopy
    $uplugin = Join-Path $srcCopy "$Name.uplugin"
    & cmd /c "`"$($Engine.RunUAT)`" BuildPlugin -Plugin=`"$uplugin`" -Package=`"$staging`" -HostPlatforms=Win64 -TargetPlatforms=Win64 > `"$log`" 2>&1"
    Remove-Item -Recurse -Force $srcCopy -ErrorAction SilentlyContinue
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $staging "Binaries\Win64\UnrealEditor-$Name.dll"))) {
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
    Copy-Item (Join-Path $Build.Dir 'DwoLensInspector.uplugin') $PluginDest
    # Disabled by default: the editor, cooks and packages never load or stage it. dwo-Lens enables it only
    # for its own commandlet runs (-EnablePlugins=DwoLensInspector), so the project needs no .uproject entry.
    $upluginPath = Join-Path $PluginDest 'DwoLensInspector.uplugin'
    $uplugin = Get-Content -Raw -Encoding UTF8 $upluginPath | ConvertFrom-Json
    $uplugin | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $false -Force
    [IO.File]::WriteAllText($upluginPath, ($uplugin | ConvertTo-Json -Depth 10), $Utf8)
    Copy-Item -Recurse (Join-Path $Build.Dir 'Binaries') $PluginDest
    if (Test-Path (Join-Path $Build.Dir 'Resources')) { Copy-Item -Recurse (Join-Path $Build.Dir 'Resources') $PluginDest }
    [IO.File]::WriteAllText((Join-Path $PluginDest $Marker), "Managed by dwo-Lens. Removed by '__dwo-lens.bat detach'.`n", $Utf8)
}

function Remove-Plugin {
    if (-not (Test-Path $PluginDest)) { return }
    if (-not (Test-Path (Join-Path $PluginDest $Marker))) {
        Fail "$PluginDest exists but is not managed by dwo-Lens. Leaving it untouched."
    }
    Remove-Item -Recurse -Force $PluginDest
    $plugins = Join-Path $Project 'Plugins'
    if ((Test-Path $plugins) -and -not (Get-ChildItem -Force $plugins)) { Remove-Item -Force $plugins }
}

# ---------------------------------------------------------------- commands
# ---------------------------------------------------------------- StateMonitor (provided tool)
function Get-StateMonitorReferences {
    # Project code that includes or calls StateMonitor. Removing the plugin would break those builds.
    $refs = @()
    foreach ($dir in @('Source', 'Plugins')) {
        $path = Join-Path $Project $dir
        if (-not (Test-Path $path)) { continue }
        $refs += Get-ChildItem -Path $path -Recurse -File -Include *.h, *.cpp, *.cs -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notlike '*__StateMonitor*' -and $_.FullName -notlike '*__DwoLens*' } |
                 Select-String -Pattern 'StateMonitor' -List | ForEach-Object { $_.Path.Substring($Project.Length + 1) + ':' + $_.LineNumber }
    }
    return $refs
}

function Find-OwnStateMonitor {
    # A StateMonitor that belongs to the project itself (not installed by dwo-Lens).
    $plugins = Join-Path $Project 'Plugins'
    if (-not (Test-Path $plugins)) { return $null }
    return Get-ChildItem -Path $plugins -Recurse -Filter 'StateMonitor.uplugin' -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notlike '*__StateMonitor*' -and $_.FullName -notlike '*__Dwo*' } | Select-Object -First 1
}

function Invoke-SmInstall {
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $own = Find-OwnStateMonitor
    if ($own) { Fail "The project already contains StateMonitor ($($own.FullName.Substring($Project.Length + 1))). Nothing to install." }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    Set-ExcludeBlock
    $build = Get-PluginBuild $engine 'StateMonitor'
    if (Test-Path $SmDest) { Remove-Item -Recurse -Force $SmDest }
    New-Item -ItemType Directory -Force $SmDest | Out-Null
    # Installed as a source plugin with prebuilt editor binaries: the editor opens without compiling, and
    # packaging (including Blueprint-only projects) builds a game target that contains StateMonitor.
    Get-ChildItem $build.Dir | Where-Object { $_.Name -ne 'Intermediate' } | ForEach-Object { Copy-Item -Recurse $_.FullName $SmDest }
    $upluginPath = Join-Path $SmDest 'StateMonitor.uplugin'
    $uplugin = Get-Content -Raw -Encoding UTF8 $upluginPath | ConvertFrom-Json
    $uplugin | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $true -Force
    $uplugin | Add-Member -NotePropertyName Installed -NotePropertyValue $false -Force
    $runtime = $Rest -contains '--runtime'
    if (-not $runtime) {
        # Default: editor and PIE only. Packaged games never load the module, so installing it cannot
        # change or break a package. (A Blueprint-only package uses the stock UnrealGame executable,
        # which cannot contain a plugin the .uproject does not list.)
        foreach ($m in $uplugin.Modules) { $m | Add-Member -NotePropertyName TargetAllowList -NotePropertyValue @('Editor') -Force }
    }
    [IO.File]::WriteAllText($upluginPath, ($uplugin | ConvertTo-Json -Depth 10), $Utf8)
    [IO.File]::WriteAllText((Join-Path $SmDest $Marker), "StateMonitor installed by dwo-Lens. Remove with '__dwo-lens.bat sm-remove'.`n", $Utf8)
    if ($runtime) {
        Say "StateMonitor installed for runtime: Plugins\__StateMonitor (UE $($engine.Version), hidden from git)"
        Say 'Packaged games include it only when the project declares it:'
        if ($uproject.IsCode) { Say '  C++ project: add "StateMonitor" to the Build.cs of the module that calls it.' }
        else { Say '  Blueprint-only project: add {"Name": "StateMonitor", "Enabled": true} to Plugins in the .uproject.' }
        Say 'Remove those entries before sm-remove or detach.'
    } else {
        Say "StateMonitor installed for editor and PIE: Plugins\__StateMonitor (UE $($engine.Version), hidden from git)"
        Say 'Packaged builds are not affected. For packaged games use: __dwo-lens.bat sm-install --runtime'
    }
    Say 'Blueprint: Submit State Snapshot / Show State Monitor nodes. C++: UStateMonitorSubsystem::Get(this)->Submit(TEXT("Id"), State).'
}

function Remove-StateMonitor([bool]$Force) {
    if (-not (Test-Path $SmDest)) { return $true }
    if (-not (Test-Path (Join-Path $SmDest $Marker))) { Fail "$SmDest is not managed by dwo-Lens. Leaving it untouched." }
    $refs = @(Get-StateMonitorReferences)
    if ($refs.Count -gt 0 -and -not $Force) {
        Say 'Project code still uses StateMonitor. Removing the plugin would break the build:'
        $refs | Select-Object -First 15 | ForEach-Object { Write-Host "    $_" }
        Say 'Remove those references first, or run again with --force.'
        return $false
    }
    Remove-Item -Recurse -Force $SmDest
    $plugins = Join-Path $Project 'Plugins'
    if ((Test-Path $plugins) -and -not (Get-ChildItem -Force $plugins)) { Remove-Item -Force $plugins }
    Say 'StateMonitor removed.'
    return $true
}

function Invoke-SmRemove {
    if (-not (Test-Path $SmDest)) { Say 'StateMonitor is not installed by dwo-Lens.'; return }
    if (-not (Remove-StateMonitor ($Rest -contains '--force'))) { exit 1 }
}

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
        schema                = 'lens.state/0.1'
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
    if ($env:DWO_LENS_HOME -and (Test-Path (Join-Path $env:DWO_LENS_HOME '.git'))) {
        $latest = & git -C $env:DWO_LENS_HOME rev-parse --short HEAD 2>$null
        if ($latest -and $latest -ne $state.systemCommit) { Say "Update available: $latest (run __dwo-lens.bat update)" }
        else { Say 'System is up to date.' }
    }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    if ($engine.Version -ne $state.engineVersion) { Say "Engine changed to $($engine.Version). Run __dwo-lens.bat update." }
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
    Check 'project is its own git repository' ($top -and (Test-OwnRepository)) 'run git init in the project'
    $excl = Get-ExcludePath
    Check 'exclude block present' ((Test-Path $excl) -and ((Get-Content -Raw $excl) -like "*$BlockStart*")) 'run __dwo-lens.bat attach'
    Check 'plugin installed' (Test-Path (Join-Path $PluginDest 'Binaries\Win64\UnrealEditor-DwoLensInspector.dll')) 'run __dwo-lens.bat attach'
    $leaks = Invoke-Git @('status', '--porcelain', '--untracked-files=all', '--', '__dwo-lens', '__dwo-lens.bat', 'Plugins/__DwoLensInspector', 'Plugins/__StateMonitor')
    Check 'dwo-lens files hidden from git' (-not $leaks) "visible to git: $leaks"
    $py = Get-Command python -ErrorAction SilentlyContinue
    Check 'python available' ($py -ne $null) 'install Python 3.10+'
    if ($uproject.IsCode) {
        Write-Host '  [note] code project: Plugins/__DwoLensInspector is prebuilt, editor-only and disabled by default.'
    }
    if (Test-Path $SmDest) { Write-Host '  [info] StateMonitor installed by dwo-Lens (Plugins/__StateMonitor)' }
    elseif (Find-OwnStateMonitor) { Write-Host '  [info] StateMonitor is part of the project itself' }
    else { Write-Host '  [info] StateMonitor not installed (optional: __dwo-lens.bat sm-install)' }
    return (-not $script:doctorFailed)
}

# ---------------------------------------------------------------- config / engine runs
function Get-ConfigFiles {
    $files = @(Join-Path $SystemDir 'config\default.json')
    $override = Join-Path $LensDir 'config.json'
    if (Test-Path $override) { $files += $override }
    return $files
}

function Get-Config {
    # Two-level merge: project config.json overrides keys inside each section of default.json.
    $cfg = Get-Content -Raw -Encoding UTF8 (Join-Path $SystemDir 'config\default.json') | ConvertFrom-Json
    $override = Join-Path $LensDir 'config.json'
    if (Test-Path $override) {
        $o = Get-Content -Raw -Encoding UTF8 $override | ConvertFrom-Json
        foreach ($section in $o.PSObject.Properties) {
            if ($cfg.PSObject.Properties[$section.Name] -and $section.Value -is [pscustomobject]) {
                foreach ($k in $section.Value.PSObject.Properties) {
                    $cfg.($section.Name) | Add-Member -NotePropertyName $k.Name -NotePropertyValue $k.Value -Force
                }
            } else {
                $cfg | Add-Member -NotePropertyName $section.Name -NotePropertyValue $section.Value -Force
            }
        }
    }
    return $cfg
}

function Invoke-Engine($Engine, [string]$Arguments, [string]$RunDir, [string]$Name, [int]$TimeoutMinutes) {
    # Runs UnrealEditor-Cmd headless with its log routed into the run folder. Returns exit code, -1 on timeout.
    $log = Join-Path $RunDir "$Name.log"
    $all = "$Arguments -unattended -nullrhi -nosplash -nopause -NoSound -ABSLOG=`"$log`""
    $p = Start-Process -FilePath $Engine.Cmd -ArgumentList $all -NoNewWindow -PassThru `
         -RedirectStandardOutput (Join-Path $RunDir "$Name.stdout.txt") -RedirectStandardError (Join-Path $RunDir "$Name.stderr.txt")
    $null = $p.Handle
    if (-not $p.WaitForExit($TimeoutMinutes * 60000)) {
        try { $p.Kill() } catch { }
        Say "$Name timed out after $TimeoutMinutes min"
        return -1
    }
    return $p.ExitCode
}

function Find-StateMonitor($Config) {
    $name = $Config.stateMonitor.pluginName
    $plugins = Join-Path $Project 'Plugins'
    if (-not (Test-Path $plugins)) { return $null }
    return Get-ChildItem -Path $plugins -Recurse -Filter "$name.uplugin" -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notlike '*__Dwo*' } | Select-Object -First 1
}

function New-RunDir([string]$Prefix) {
    $runId = "$Prefix-" + (Get-Date).ToString('yyyyMMdd-HHmmss')
    $dir = Join-Path $DataDir "runs\$runId"
    New-Item -ItemType Directory -Force $dir | Out-Null
    return [pscustomobject]@{ Id = $runId; Dir = $dir }
}

function Get-PreviousOutput([string]$Prefix, [string]$File, [string]$ExceptId) {
    $runs = Join-Path $DataDir 'runs'
    if (-not (Test-Path $runs)) { return $null }
    return Get-ChildItem $runs -Directory -Filter "$Prefix-*" | Where-Object { $_.Name -ne $ExceptId } |
           Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName "out\$File" } |
           Where-Object { Test-Path $_ } | Select-Object -First 1
}

function Write-Run($Run, [string]$Line, $Stations, [string]$Final, $Engine) {
    $state = Read-State
    Write-Json (Join-Path $Run.Dir 'run.json') ([ordered]@{
        schema = 'lens.run/0.1'; runId = $Run.Id; line = $Line; project = $state.project
        engineVersion = $Engine.Version; systemCommit = Get-SystemCommit
        finishedAt = (Get-Date).ToString('o'); stations = $Stations; final = $Final
    })
}

function Invoke-Verify {
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $cfg = Get-Config
    $v = $cfg.verify
    $run = New-RunDir 'verify'
    $stations = New-Object System.Collections.Generic.List[object]
    $up = "`"$($uproject.Path)`""

    if ($v.compileBlueprints) {
        Say "[$($run.Id)] V1 CompileAllBlueprints"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-Engine $engine "$up -run=CompileAllBlueprints -ShowResultsOnly" $run.Dir 'compile' $v.timeoutMinutes
        $stations.Add([ordered]@{ id = 'V1'; name = 'compile-blueprints'; exitCode = $code; ms = $sw.ElapsedMilliseconds; verdict = $null })
    }
    if ($v.dataValidation) {
        Say "[$($run.Id)] V2 DataValidation"
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-Engine $engine "$up -run=DataValidation" $run.Dir 'datavalidation' $v.timeoutMinutes
        $stations.Add([ordered]@{ id = 'V2'; name = 'data-validation'; exitCode = $code; ms = $sw.ElapsedMilliseconds; verdict = $null })
    }
    if ($v.automation) {
        $filters = @($v.automationFilters)
        $sm = Find-StateMonitor $cfg
        if ($sm) {
            $filters += $cfg.stateMonitor.testFilter
            Say "StateMonitor detected ($($sm.FullName.Substring($Project.Length + 1))): adding '$($cfg.stateMonitor.testFilter)' tests"
        }
        $filterText = ($filters | Select-Object -Unique) -join '+'
        Say "[$($run.Id)] V3 Automation RunTests $filterText"
        $report = Join-Path $run.Dir 'automation'
        New-Item -ItemType Directory -Force $report | Out-Null
        $sw = [Diagnostics.Stopwatch]::StartNew()
        $code = Invoke-Engine $engine "$up -ExecCmds=`"Automation RunTests $filterText;Quit`" -testexit=`"Automation Test Queue Empty`" -ReportExportPath=`"$report`"" $run.Dir 'automation' $v.timeoutMinutes
        $stations.Add([ordered]@{ id = 'V3'; name = 'automation'; filter = $filterText; exitCode = $code; ms = $sw.ElapsedMilliseconds; verdict = $null })
    }

    & python (Join-Path $SystemDir 'tools\lens_verify.py') verify --run $run.Dir
    $result = Get-Content -Raw -Encoding UTF8 (Join-Path $run.Dir 'out\verify.json') | ConvertFrom-Json
    foreach ($s in $stations) {
        $step = $result.steps | Where-Object { $_.id -eq $s.id } | Select-Object -First 1
        if ($step) { $s.verdict = $step.verdict }
    }
    Write-Run $run 'verify' $stations $result.verdict $engine
    Say "[$($run.Id)] verdict: $($result.verdict)"
    Say "Report: $(Join-Path $run.Dir 'out\report.html')"
    if ($result.verdict -eq 'fail') { exit 2 }
}

function Invoke-Logs {
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $cfg = Get-Config
    $run = New-RunDir 'logs'
    $logDir = Join-Path $Project 'Saved\Logs'
    $files = @()
    if (Test-Path $logDir) {
        $files = @(Get-ChildItem $logDir -Filter *.log -File | Sort-Object LastWriteTime -Descending |
                   Select-Object -First $cfg.logs.maxFiles | ForEach-Object { $_.FullName })
    }
    Say "[$($run.Id)] ST-4 logs: $($files.Count) log files"
    $pyArgs = @((Join-Path $SystemDir 'tools\lens_verify.py'), 'logs', '--run', $run.Dir, '--crashes', (Join-Path $Project 'Saved\Crashes'))
    if ($files.Count -gt 0) { $pyArgs += '--files'; $pyArgs += $files }
    $baseline = Get-PreviousOutput 'logs' 'logs.json' $run.Id
    if ($baseline) { $pyArgs += @('--baseline', $baseline) }
    $ignore = @($cfg.logs.ignoreCategories)
    if ($ignore.Count -gt 0) { $pyArgs += '--ignore'; $pyArgs += $ignore }
    & python @pyArgs
    $summary = Get-Content -Raw -Encoding UTF8 (Join-Path $run.Dir 'out\logs.json') | ConvertFrom-Json
    Write-Run $run 'logs' @([ordered]@{ id = 'ST-4'; name = 'logs'; verdict = $summary.verdict; files = $files.Count }) $summary.verdict $engine
    Say "[$($run.Id)] verdict: $($summary.verdict)"
    Say "Report: $(Join-Path $run.Dir 'out\report.html')"
}

function Invoke-Tools {
    $catalog = Get-Content -Raw -Encoding UTF8 (Join-Path $SystemDir 'tools\catalog.json') | ConvertFrom-Json
    foreach ($t in $catalog.tools) {
        Write-Host ("  {0,-5} {1,-24} {2,-6} {3,-10} {4}" -f $t.station, $t.id, $t.kind, $t.status, $t.command)
    }
}

function Invoke-Analyze {
    $state = Read-State
    if (-not $state) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $runId = 'run-' + (Get-Date).ToString('yyyyMMdd-HHmmss')
    $runDir = Join-Path $DataDir "runs\$runId"
    $rawDir = Join-Path $runDir 'raw'
    $outDir = Join-Path $runDir 'out'
    New-Item -ItemType Directory -Force $rawDir, $outDir | Out-Null
    $stations = New-Object System.Collections.Generic.List[object]

    Say "[$runId] ST-1 collect: running DwoLensInspector commandlet"
    $log = Join-Path $runDir 'commandlet.log'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & cmd /c "`"$($engine.Cmd)`" `"$($uproject.Path)`" -run=DwoLensInspector -EnablePlugins=DwoLensInspector -LensOut=`"$rawDir`" -ABSLOG=`"$(Join-Path $runDir 'editor.log')`" -unattended -nullrhi -nosplash -nopause -NoSound -stdout -FullStdOutLogOutput > `"$log`" 2>&1"
    $code = $LASTEXITCODE
    $sw.Stop()
    $collected = Test-Path (Join-Path $rawDir 'meta.json')
    $stations.Add([ordered]@{ id = 'ST-1'; name = 'collect'; verdict = $(if ($collected) { 'pass' } else { 'fail' }); exitCode = $code; ms = $sw.ElapsedMilliseconds; log = 'commandlet.log' })
    if (-not $collected) { Write-Json (Join-Path $runDir 'run.json') @{ runId = $runId; stations = $stations; final = 'fail' }; Fail "Commandlet produced no output. See $log" }

    Say "[$runId] ST-2/3 analyze and gate"
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & python (Join-Path $SystemDir 'tools\lens_analyze.py') --raw $rawDir --project $uproject.Path --out $outDir --run-id $runId --config @(Get-ConfigFiles)
    $pyCode = $LASTEXITCODE
    $sw.Stop()
    $findingsPath = Join-Path $outDir 'findings.json'
    $verdict = 'fail'
    if ($pyCode -eq 0 -and (Test-Path $findingsPath)) { $verdict = (Get-Content -Raw -Encoding UTF8 $findingsPath | ConvertFrom-Json).verdict }
    $stations.Add([ordered]@{ id = 'ST-2'; name = 'analyze+gate'; verdict = $verdict; exitCode = $pyCode; ms = $sw.ElapsedMilliseconds })

    $run = [ordered]@{
        schema = 'lens.run/0.1'; runId = $runId; line = 'architecture-report'
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
    if (-not (Remove-StateMonitor ($Rest -contains '--force'))) { Fail 'Detach stopped. StateMonitor is still in use.' }
    Remove-Plugin
    Clear-ExcludeBlock
    if ($keepData -and (Test-Path $DataDir)) {
        $dest = Join-Path $env:LOCALAPPDATA ("dwo-Lens\detached\" + (Split-Path -Leaf $Project) + '-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        Move-Item $DataDir $dest
        Say "Analysis data kept at $dest"
    }
    Set-Location $Project
    if (Test-Path $LensDir) { Remove-Item -Recurse -Force $LensDir }

    # __dwo-lens.bat deletes itself after this script returns; exclude it from the comparison.
    $after = (@((Get-ProjectStatus) -split "`n") | Where-Object { $_ -and $_ -notmatch '__dwo-lens\.bat$' }) -join "`n"
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
    'verify'  { Invoke-Verify }
    'logs'    { Invoke-Logs }
    'tools'   { Invoke-Tools }
    'sm-install' { Invoke-SmInstall }
    'sm-remove'  { Invoke-SmRemove }
    'gui'     { & (Join-Path $SystemDir 'tools\gui.ps1') -SystemDir $SystemDir -Project $Project -LensHome $env:DWO_LENS_HOME }
    'detach'  { Invoke-Detach }
}

# dwo-Lens control script. Runs from the dwo-Lens install folder (a git clone or an extracted zip);
# invoked by dwo-Lens.bat, the GUI, or the project's __dwo-lens.bat launcher.
# Windows PowerShell 5.1 compatible. Never modifies tracked project files.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('attach', 'update', 'status', 'analyze', 'verify', 'logs', 'smoke', 'perf', 'sizes', 'trace', 'play', 'tools', 'gui', 'sm-install', 'sm-remove', 'doctor', 'uninstall', 'detach')]
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
$ManagedPaths = @('__dwo-lens/', '__dwo-lens.bat', 'Plugins/__DwoLensInspector/', 'Plugins/__StateMonitor/')
$LauncherPath = Join-Path $Project '__dwo-lens.bat'
$LedgerPath  = Join-Path $LensDir 'created-files.txt'   # files dwo-Lens caused outside __dwo-lens (removed by uninstall)

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
    # git is optional: a project without version control (or a PC without git) simply has no repository.
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { return $null }
    $ErrorActionPreference = 'Continue'
    $out = & git -C $Project @GitArgs 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return $out
}

# How the project is version controlled:
#   own    - the project folder is a git repository root
#   parent - the project sits inside a larger git repository (paths are prefixed in that repository's exclude)
#   none   - no git; nothing to hide, so no exclude is written
function Get-Vcs {
    $top = Invoke-Git @('rev-parse', '--show-toplevel')
    if (-not $top) { return [pscustomobject]@{ Kind = 'none'; Top = $null; Prefix = ''; Exclude = $null } }
    # Prefix comparison stays correct through junctions, symlinks and redirected profile paths.
    $prefix = ((& git -C $Project rev-parse --show-prefix 2>$null) | Out-String).Trim()
    $excl = Invoke-Git @('rev-parse', '--path-format=absolute', '--git-path', 'info/exclude')
    return [pscustomobject]@{ Kind = $(if ($prefix) { 'parent' } else { 'own' }); Top = "$top"; Prefix = $prefix
                              Exclude = $(if ($excl) { ("$excl" -replace '/', '\') } else { $null }) }
}

function Get-ExcludeLines([string]$Path) {
    if (-not (Test-Path $Path)) { return @() }
    return @([IO.File]::ReadAllLines($Path))
}

# One block per project, so several projects inside one parent repository keep separate entries.
function Get-BlockId($Vcs) { if ($Vcs.Prefix) { return $Vcs.Prefix } else { return '.' } }

function Set-ExcludeBlock {
    $vcs = Get-Vcs
    if ($vcs.Kind -eq 'none' -or -not $vcs.Exclude) { return $vcs }
    $id = Get-BlockId $vcs
    $lines = Get-ExcludeLines $vcs.Exclude
    $kept = Remove-Block $lines $id
    $block = @("# >>> dwo-lens managed: $id (do not edit) >>>") + @($ManagedPaths | ForEach-Object { '/' + $vcs.Prefix + $_ }) + @($BlockEnd)
    $all = @($kept | Where-Object { $_ -ne $null }) + @('') + $block
    $dir = Split-Path -Parent $vcs.Exclude
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [IO.File]::WriteAllText($vcs.Exclude, (($all -join "`n").Trim() + "`n"), $Utf8)
    return $vcs
}

# Removes this project's block. The unnamed block of older versions belongs to the repository root project.
function Remove-Block([string[]]$Lines, [string]$Id) {
    $out = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in $Lines) {
        if ($line -like '# >>> dwo-lens managed*') {
            $legacy = $line -notlike '# >>> dwo-lens managed: *'
            $mine = ($legacy -and $Id -eq '.') -or $line -like "# >>> dwo-lens managed: $Id *"
            if ($mine) { $inside = $true; continue }
        }
        if ($inside) {
            if ($line -like '# <<< dwo-lens managed*') { $inside = $false }
            continue
        }
        $out.Add($line)
    }
    while ($out.Count -gt 0 -and [string]::IsNullOrWhiteSpace($out[$out.Count - 1])) { $out.RemoveAt($out.Count - 1) }
    return ,$out.ToArray()
}

function Clear-ExcludeBlock {
    $vcs = Get-Vcs
    if ($vcs.Kind -eq 'none' -or -not $vcs.Exclude -or -not (Test-Path $vcs.Exclude)) { return $false }
    $lines = Get-ExcludeLines $vcs.Exclude
    $kept = Remove-Block $lines (Get-BlockId $vcs)
    if ($kept.Count -eq ($lines | Where-Object { $_ -ne $null }).Count) { return $false }
    $text = ''
    if ($kept.Count -gt 0) { $text = ($kept -join "`n") + "`n" }
    [IO.File]::WriteAllText($vcs.Exclude, $text, $Utf8)
    return $true
}

function Test-BlockPresent {
    $vcs = Get-Vcs
    if ($vcs.Kind -eq 'none' -or -not $vcs.Exclude -or -not (Test-Path $vcs.Exclude)) { return $false }
    $id = Get-BlockId $vcs
    $text = Get-Content -Raw $vcs.Exclude
    return ($text -like "*# >>> dwo-lens managed: $id *") -or ($id -eq '.' -and $text -like '*# >>> dwo-lens managed (do not edit)*')
}

# Folders the engine itself rewrites whenever an editor or commandlet runs (ignored by the standard UE .gitignore).
$VolatileDirs = @('Saved/', 'Intermediate/', 'DerivedDataCache/', 'Binaries/', '.vs/')

function Get-ProjectStatus {
    # Porcelain status of the project folder, used to prove attach/uninstall leave no trace.
    # Engine work folders are excluded: running the editor always touches them.
    # Porcelain paths are relative to the repository root; strip the project prefix (parent repository case).
    $prefix = ((& git -C $Project rev-parse --show-prefix 2>$null) | Out-String).Trim()
    $s = Invoke-Git @('status', '--porcelain', '--untracked-files=all', '--', '.')
    if (-not $s) { return '' }
    $kept = @($s) | Where-Object {
        $path = $_.Substring(3).Trim('"')
        if ($prefix -and $path.StartsWith($prefix)) { $path = $path.Substring($prefix.Length) }
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
    # and the install folder must stay as shipped.
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
    [IO.File]::WriteAllText((Join-Path $PluginDest $Marker), "Managed by dwo-Lens. Removed by '__dwo-lens.bat uninstall'.`n", $Utf8)
}

function Remove-EmptyPluginsFolder {
    # Remove Plugins\ only when attach created it (it did not exist before) and nothing else is in it now.
    $plugins = Join-Path $Project 'Plugins'
    $state = Read-State
    $existed = $state -and $state.PSObject.Properties['pluginsFolderExisted'] -and [bool]$state.pluginsFolderExisted
    if (-not $existed -and (Test-Path $plugins) -and -not (Get-ChildItem -Force $plugins)) { Remove-Item -Force $plugins }
}

function Remove-Plugin {
    if (-not (Test-Path $PluginDest)) { return }
    if (-not (Test-Path (Join-Path $PluginDest $Marker))) {
        Fail "$PluginDest exists but is not managed by dwo-Lens. Leaving it untouched."
    }
    Remove-Item -Recurse -Force $PluginDest
    Remove-EmptyPluginsFolder
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
    if (-not (Read-State)) { Fail 'Not attached. Run attach first.' }
    $own = Find-OwnStateMonitor
    if ($own) { Fail "The project already contains StateMonitor ($($own.FullName.Substring($Project.Length + 1))). Nothing to install." }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $null = Set-ExcludeBlock
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
        Say "StateMonitor installed for runtime: Plugins\__StateMonitor (UE $($engine.Version))"
        Say 'Packaged games include it only when the project declares it:'
        if ($uproject.IsCode) { Say '  C++ project: add "StateMonitor" to the Build.cs of the module that calls it.' }
        else { Say '  Blueprint-only project: add {"Name": "StateMonitor", "Enabled": true} to Plugins in the .uproject.' }
        Say 'Remove those entries before sm-remove or uninstall.'
    } else {
        Say "StateMonitor installed for editor and PIE: Plugins\__StateMonitor (UE $($engine.Version))"
        Say 'Packaged builds are not affected. For packaged games use: __dwo-lens.bat sm-install --runtime'
    }
    Say 'Live view: open dwo-Lens > State Monitor, then Play in the editor or run __dwo-lens.bat play. No project code needed.'
    Say 'Optional: Blueprint Watch State Object / Submit State Snapshot. C++: UStateMonitorSubsystem::Get(this)->Submit(TEXT("Id"), State).'
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
    Remove-EmptyPluginsFolder
    Say 'StateMonitor removed.'
    return $true
}

function Invoke-SmRemove {
    if (-not (Test-Path $SmDest)) { Say 'StateMonitor is not installed by dwo-Lens.'; return }
    if (-not (Remove-StateMonitor ($Rest -contains '--force'))) { exit 1 }
}

function Get-SystemCommit {
    # Version of the install folder: VERSION file, plus the commit when it is a git clone.
    $versionFile = Join-Path $SystemDir 'VERSION'
    $v = if (Test-Path $versionFile) { (Get-Content -Raw $versionFile).Trim() } else { 'dev' }
    if (Test-Path (Join-Path $SystemDir '.git')) {
        $c = & git -C $SystemDir rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $c) { return "$v+$c" }
    }
    return $v
}

function Add-Created([string]$Path) {
    # Remembers a file dwo-Lens caused outside __dwo-lens so uninstall can offer to remove it.
    if (-not $Path -or -not (Test-Path $LensDir)) { return }
    [IO.File]::AppendAllText($LedgerPath, $Path.Substring($Project.Length + 1) + "`n", $Utf8)
}

function Install-Launcher {
    # The project's __dwo-lens.bat: a small launcher that finds this install folder (home.txt) and runs lens.ps1.
    $src = Join-Path $SystemDir '__dwo-lens.bat'
    $existing = if (Test-Path $LauncherPath) { [IO.File]::ReadAllBytes($LauncherPath) } else { $null }
    $new = [IO.File]::ReadAllBytes($src)
    if (-not $existing -or [Convert]::ToBase64String($existing) -ne [Convert]::ToBase64String($new)) {
        # A running __dwo-lens.bat is read line by line by cmd; replace it only after it finished.
        if ($env:DWO_LENS_FROM_BAT -eq '1') { Copy-Item $src (Join-Path $LensDir 'launcher.next') -Force }
        else { Copy-Item $src $LauncherPath -Force }
    }
}

function Invoke-Attach([string]$Mode) {
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $previous = Read-State
    $baseline = $null
    if ($previous -and $previous.gitStatusBeforeAttach -ne $null) { $baseline = $previous.gitStatusBeforeAttach }
    $pluginsExisted = if ($previous -and $previous.PSObject.Properties['pluginsFolderExisted']) { [bool]$previous.pluginsFolderExisted } else { Test-Path (Join-Path $Project 'Plugins') }

    # Hide everything before it exists (git only).
    if ($baseline -eq $null -and (Get-Vcs).Kind -ne 'none') { $baseline = Get-ProjectStatus }
    $vcs = Set-ExcludeBlock
    switch ($vcs.Kind) {
        'own'    { Say "git repository: dwo-Lens files are hidden through .git\info\exclude (local only, never committed)." }
        'parent' { Say "The project is inside the git repository $($vcs.Top). dwo-Lens files are hidden there under /$($vcs.Prefix)." }
        'none'   { Say 'No version control: nothing to hide, so no exclude entry is written.' }
    }

    New-Item -ItemType Directory -Force $LensDir | Out-Null
    # Older versions kept a git clone of the system inside the project. The system now runs from the install folder.
    $legacy = Join-Path $LensDir 'system'
    if (Test-Path $legacy) {
        Get-ChildItem $legacy -Recurse -Force -File | ForEach-Object { if ($_.IsReadOnly) { $_.IsReadOnly = $false } }
        Remove-Item -Recurse -Force $legacy
        Say 'Removed the old system copy (__dwo-lens\system); the system now runs from the dwo-Lens folder.'
    }
    [IO.File]::WriteAllText((Join-Path $LensDir 'home.txt'), $SystemDir, $Utf8)
    Install-Launcher

    $build = Get-PluginBuild $engine
    Install-Plugin $build

    $state = [ordered]@{
        schema                = 'lens.state/0.2'
        project               = $uproject.Name
        projectType           = $(if ($uproject.IsCode) { 'code' } else { 'blueprint-only' })
        engineVersion         = $engine.Version
        engineRoot            = $engine.Root
        systemCommit          = Get-SystemCommit
        systemHome            = $SystemDir
        pluginHash            = $build.Hash
        vcs                   = $vcs.Kind
        vcsRoot               = $vcs.Top
        excludeFile           = $vcs.Exclude
        pluginsFolderExisted  = $pluginsExisted
        attachedAt            = $(if ($previous) { $previous.attachedAt } else { (Get-Date).ToString('o') })
        updatedAt             = (Get-Date).ToString('o')
        gitStatusBeforeAttach = $baseline
    }
    Write-Json $StatePath $state
    Say "$Mode complete: $($uproject.Name) / UE $($engine.Version) / dwo-Lens $($state.systemCommit)"
    Say "Open the control panel: dwo-Lens.bat (or __dwo-lens.bat in the project). Remove everything: __dwo-lens.bat uninstall"
    Invoke-Doctor | Out-Null
}

function Invoke-Status {
    $state = Read-State
    if (-not $state) { Say 'Not attached.'; return }
    Say "Project        : $($state.project) ($($state.projectType))"
    Say "Engine         : UE $($state.engineVersion)"
    Say "dwo-Lens       : $(Get-SystemCommit) at $SystemDir"
    if ($state.systemCommit -ne (Get-SystemCommit)) { Say "Attached with $($state.systemCommit). Run update to refresh the plugin and launcher." }
    Say "Version control: $((Get-Vcs).Kind)"
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
    $vcs = Get-Vcs
    if ($vcs.Kind -eq 'none') { Write-Host '  [info] no version control: exclude not needed' }
    else {
        if ($vcs.Kind -eq 'parent') { Write-Host "  [info] inside the git repository $($vcs.Top) (entries prefixed /$($vcs.Prefix))" }
        Check 'exclude block present' (Test-BlockPresent) 'run attach again'
        $leaks = Invoke-Git @('status', '--porcelain', '--untracked-files=all', '--', '__dwo-lens', '__dwo-lens.bat', 'Plugins/__DwoLensInspector', 'Plugins/__StateMonitor')
        Check 'dwo-lens files hidden from git' (-not $leaks) "visible to git: $leaks"
    }
    Check 'plugin installed' (Test-Path (Join-Path $PluginDest 'Binaries\Win64\UnrealEditor-DwoLensInspector.dll')) 'run attach again'
    Check 'project launcher' (Test-Path $LauncherPath) 'run attach again'
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

# ---------------------------------------------------------------- runtime lines (smoke, perf, sizes, trace, play)
function Get-EditorExe($Engine) { return (Join-Path $Engine.Root 'Engine\Binaries\Win64\UnrealEditor.exe') }

function Get-MapArg([string]$Fallback) {
    # First non-option argument after the command, e.g. __dwo-lens.bat perf /Game/Maps/L_Main
    $m = @($Rest | Where-Object { $_ -and -not $_.StartsWith('--') }) | Select-Object -First 1
    if ($m) { return $m }
    if ($Fallback) { return $Fallback }
    return (Get-DefaultMap)
}

function Get-DefaultMap {
    # GameDefaultMap from Config\DefaultEngine.ini ("/Game/Maps/Main.Main" -> "/Game/Maps/Main").
    $ini = Join-Path $Project 'Config\DefaultEngine.ini'
    if (-not (Test-Path $ini)) { return '' }
    foreach ($line in [IO.File]::ReadAllLines($ini)) {
        if ($line -match '^\s*GameDefaultMap\s*=\s*(\S+)') { return ($Matches[1] -replace '\.[^/]*$', '') }
    }
    return ''
}

function Get-ConfigValue($Section, [string]$Key, $Default) {
    if ($Section -and $Section.PSObject.Properties[$Key] -and $Section.$Key -ne $null -and "$($Section.$Key)" -ne '') { return $Section.$Key }
    return $Default
}

function Invoke-Timed([string]$Exe, [string]$Arguments, [string]$RunDir, [string]$Name, [int]$Seconds, [switch]$Window) {
    # Runs a game process for a fixed time, then stops it: window close first (windowed runs), then the whole
    # process tree. ExitCode is $null when dwo-Lens stopped the process, the real code when it exited on its own.
    $started = Get-Date
    $sp = @{ FilePath = $Exe; ArgumentList = $Arguments; PassThru = $true }
    if (-not $Window) {
        $sp.NoNewWindow = $true
        $sp.RedirectStandardOutput = Join-Path $RunDir "$Name.stdout.txt"
        $sp.RedirectStandardError = Join-Path $RunDir "$Name.stderr.txt"
    }
    $p = Start-Process @sp
    $null = $p.Handle
    $killed = $false
    if (-not $p.WaitForExit($Seconds * 1000)) {
        $killed = $true
        if ($Window) { try { [void]$p.CloseMainWindow() } catch { }; [void]$p.WaitForExit(15000) }
        if (-not $p.HasExited) { & taskkill /PID $p.Id /T /F 2>$null | Out-Null; [void]$p.WaitForExit(15000) }
    }
    $ended = Get-Date
    $code = $null
    if (-not $killed) { $code = $p.ExitCode }
    return [pscustomobject]@{
        ExitCode = $code; Killed = $killed; Seconds = ($ended - $started).TotalSeconds
        StartedAt = [DateTimeOffset]::new($started).ToUnixTimeSeconds(); EndedAt = [DateTimeOffset]::new($ended).ToUnixTimeSeconds()
    }
}

function Read-LineResult([string]$Path) {
    if (Test-Path $Path) { return (Get-Content -Raw -Encoding UTF8 $Path | ConvertFrom-Json) }
    return [pscustomobject]@{ verdict = 'fail' }
}

function Complete-Line($Run, [string]$Line, $Stations, $Result, $Engine) {
    Write-Run $Run $Line $Stations $Result.verdict $Engine
    Say "[$($Run.Id)] verdict: $($Result.verdict)"
    Say "Report: $(Join-Path $Run.Dir 'out\report.html')"
    if ($Result.verdict -eq 'fail') { exit 2 }
}

function Invoke-Smoke {
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $cfg = Get-Config
    $seconds = [int](Get-ConfigValue $cfg.smoke 'seconds' 15)
    $run = New-RunDir 'smoke'
    $raw = Join-Path $run.Dir 'raw'
    New-Item -ItemType Directory -Force $raw | Out-Null
    $py = Join-Path $SystemDir 'tools\lens_runtime.py'
    $mapsFile = Join-Path $raw 'maps.json'
    & python $py maps --project $Project --out $mapsFile --config @(Get-ConfigFiles)
    $maps = @()
    if (Test-Path $mapsFile) { foreach ($m in (Get-Content -Raw -Encoding UTF8 $mapsFile | ConvertFrom-Json)) { $maps += [string]$m } }
    Say "[$($run.Id)] smoke: $($maps.Count) maps, $seconds s each"

    $entries = New-Object System.Collections.Generic.List[object]
    $stations = New-Object System.Collections.Generic.List[object]
    $i = 0
    foreach ($m in $maps) {
        $i++
        $name = "smoke-$i"
        $log = Join-Path $raw "$name.log"
        Say "[$($run.Id)] SM-$i $m"
        $r = Invoke-Timed $engine.Cmd "`"$($uproject.Path)`" `"$m`" -game -nullrhi -nosound -unattended -nosplash -nopause -log -ABSLOG=`"$log`"" $raw $name $seconds
        $entries.Add([ordered]@{ map = $m; log = $log; exitCode = $r.ExitCode; killed = $r.Killed; seconds = [math]::Round($r.Seconds, 1)
                                 startedAt = $r.StartedAt; endedAt = $r.EndedAt })
        $stations.Add([ordered]@{ id = "SM-$i"; name = $m; exitCode = $r.ExitCode; ms = [int]($r.Seconds * 1000); verdict = $null; log = "raw\$name.log" })
    }
    $runsFile = Join-Path $raw 'smoke-runs.json'
    if ($entries.Count -eq 0) { [IO.File]::WriteAllText($runsFile, '[]', $Utf8) }
    else { [IO.File]::WriteAllText($runsFile, (ConvertTo-Json -InputObject $entries.ToArray() -Depth 5), $Utf8) }

    & python $py smoke --run $run.Dir --project $Project --config @(Get-ConfigFiles)
    $result = Read-LineResult (Join-Path $run.Dir 'out\smoke.json')
    for ($k = 0; $k -lt $stations.Count; $k++) {
        if ($result.maps -and $k -lt @($result.maps).Count) { $stations[$k].verdict = @($result.maps)[$k].verdict }
    }
    Complete-Line $run 'smoke' $stations $result $engine
}

function Invoke-Perf {
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $cfg = Get-Config
    $p = $cfg.perf
    $map = Get-MapArg (Get-ConfigValue $p 'map' '')
    $nullrhi = [bool](Get-ConfigValue $p 'nullrhi' $false)
    $frames = [int](Get-ConfigValue $p 'frames' 600) + [int](Get-ConfigValue $p 'warmupFrames' 60)
    $timeout = [int](Get-ConfigValue $p 'timeoutSeconds' 600)
    $run = New-RunDir 'perf'
    $raw = Join-Path $run.Dir 'raw'
    $out = Join-Path $run.Dir 'out'
    New-Item -ItemType Directory -Force $raw, $out | Out-Null
    $stations = New-Object System.Collections.Generic.List[object]

    # CSV profiler: the capture starts on the first frame after the map loaded (-ExecCmds run then) and the
    # game exits by itself when the frame count is reached (csvprofile exitoncompletion / -ExitAfterCsvProfiling).
    $csvName = "$($run.Id).csv"
    $exec = "csvprofile startfile=$csvName,csvprofile exitoncompletion,csvprofile frames=$frames"
    $log = Join-Path $raw 'perf.log'
    $common = "`"$($uproject.Path)`" `"$map`" -game -nosound -unattended -nosplash -nopause -log -ABSLOG=`"$log`" -csvCompression=0 -ExitAfterCsvProfiling -ExecCmds=`"$exec`""
    Say "[$($run.Id)] P1 capture $frames frames on $map ($(if ($nullrhi) { 'nullrhi' } else { 'rendered' }))"
    if ($nullrhi) { $r = Invoke-Timed $engine.Cmd "$common -nullrhi" $raw 'perf' $timeout }
    else {
        $resX = [int](Get-ConfigValue $p 'resX' 960); $resY = [int](Get-ConfigValue $p 'resY' 540)
        $r = Invoke-Timed (Get-EditorExe $engine) "$common -windowed -ResX=$resX -ResY=$resY" $raw 'perf' $timeout -Window
    }
    $csvDir = Join-Path $Project 'Saved\Profiling\CSV'
    $csv = Join-Path $csvDir $csvName
    if (-not (Test-Path $csv) -and (Test-Path $csvDir)) {
        $since = [DateTimeOffset]::FromUnixTimeSeconds($r.StartedAt).LocalDateTime
        $found = Get-ChildItem $csvDir -Filter *.csv -File | Where-Object { $_.LastWriteTime -ge $since } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($found) { $csv = $found.FullName }
    }
    $copied = Join-Path $out 'perf.csv'
    if (Test-Path $csv) { Copy-Item $csv $copied -Force; Add-Created $csv }
    if ($r.Killed) { Say "P1 did not finish in $timeout s; stopped" }
    $stations.Add([ordered]@{ id = 'P1'; name = 'csv-capture'; exitCode = $r.ExitCode; ms = [int]($r.Seconds * 1000)
                              verdict = $(if (Test-Path $copied) { 'pass' } else { 'fail' }); log = 'raw\perf.log' })

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $pyArgs = @((Join-Path $SystemDir 'tools\lens_runtime.py'), 'perf', '--run', $run.Dir, '--map', $map, '--config') + @(Get-ConfigFiles)
    if (Test-Path $copied) { $pyArgs += @('--csv', $copied) }
    if ($r.ExitCode -ne $null) { $pyArgs += @('--exit-code', "$($r.ExitCode)") }
    & python @pyArgs
    $result = Read-LineResult (Join-Path $out 'perf.json')
    $stations.Add([ordered]@{ id = 'P2'; name = 'frame-stats'; ms = $sw.ElapsedMilliseconds; verdict = $result.verdict })
    Complete-Line $run 'perf' $stations $result $engine
}

function Invoke-Sizes {
    $state = Read-State
    if (-not $state) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $run = New-RunDir 'sizes'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $pyArgs = @((Join-Path $SystemDir 'tools\lens_runtime.py'), 'sizes', '--run', $run.Dir, '--project', $Project, '--config') + @(Get-ConfigFiles)
    $graph = Get-PreviousOutput 'run' 'graph.json' ''
    if ($graph) { $pyArgs += @('--graph', $graph) } else { Say 'No analyze run yet: hard-reference closure weights skipped (run __dwo-lens.bat analyze).' }
    Say "[$($run.Id)] Z1 content sizes"
    & python @pyArgs
    $result = Read-LineResult (Join-Path $run.Dir 'out\sizes.json')
    $stations = @([ordered]@{ id = 'Z1'; name = 'content-sizes'; ms = $sw.ElapsedMilliseconds; verdict = $result.verdict; graph = $graph })
    # No engine run: the version recorded at attach is enough for run.json.
    Complete-Line $run 'sizes' $stations $result ([pscustomobject]@{ Version = $state.engineVersion })
}

function Invoke-Trace {
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $cfg = Get-Config
    $t = $cfg.trace
    $map = Get-MapArg (Get-ConfigValue $cfg.perf 'map' '')
    $seconds = [int](Get-ConfigValue $t 'seconds' 20)
    $channels = [string](Get-ConfigValue $t 'channels' 'default,counters,loadtime,task')
    $nullrhi = [bool](Get-ConfigValue $t 'nullrhi' $false)
    $run = New-RunDir 'trace'
    $raw = Join-Path $run.Dir 'raw'
    $out = Join-Path $run.Dir 'out'
    New-Item -ItemType Directory -Force $raw, $out | Out-Null
    $file = Join-Path $out 'trace.utrace'
    $log = Join-Path $raw 'trace.log'
    $common = "`"$($uproject.Path)`" `"$map`" -game -nosound -unattended -nosplash -nopause -log -ABSLOG=`"$log`" -trace=$channels -tracefile=`"$file`" -statnamedevents"
    Say "[$($run.Id)] T1 trace $seconds s on $map (channels $channels)"
    if ($nullrhi) { $r = Invoke-Timed $engine.Cmd "$common -nullrhi" $raw 'trace' $seconds }
    else { $r = Invoke-Timed (Get-EditorExe $engine) "$common -windowed -ResX=960 -ResY=540" $raw 'trace' $seconds -Window }
    $insights = Join-Path $engine.Root 'Engine\Binaries\Win64\UnrealInsights.exe'
    $pyArgs = @((Join-Path $SystemDir 'tools\lens_runtime.py'), 'trace', '--run', $run.Dir, '--file', $file, '--seconds', "$([math]::Round($r.Seconds, 1))", '--config') + @(Get-ConfigFiles)
    if (Test-Path $insights) { $pyArgs += @('--insights', $insights) }
    if (-not $r.Killed) { $pyArgs += @('--exited', '--exit-code', "$($r.ExitCode)") }
    & python @pyArgs
    $result = Read-LineResult (Join-Path $out 'trace.json')
    $stations = @([ordered]@{ id = 'T1'; name = 'insights-trace'; exitCode = $r.ExitCode; ms = [int]($r.Seconds * 1000); verdict = $result.verdict; channels = $channels })
    Complete-Line $run 'trace' $stations $result $engine
}

function Invoke-Play {
    # Not a run: starts the game detached (e.g. for the StateMonitor viewer) and returns immediately.
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-lens.bat attach first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $map = @($Rest | Where-Object { $_ -and -not $_.StartsWith('--') }) | Select-Object -First 1
    $arguments = "`"$($uproject.Path)`""
    if ($map) { $arguments += " `"$map`"" }
    $arguments += ' -game -windowed -ResX=1280 -ResY=720 -log'
    $p = Start-Process -FilePath (Get-EditorExe $engine) -ArgumentList $arguments -PassThru
    Say "play: started $(if ($map) { $map } else { 'default map' }) (pid $($p.Id))"
    Write-Output "pid=$($p.Id)"
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

function Get-UninstallInventory {
    # What attach and the lines left in the project, grouped for the keep/delete question.
    $runs = @()
    $runsDir = Join-Path $DataDir 'runs'
    if (Test-Path $runsDir) { $runs = @(Get-ChildItem $runsDir -Directory) }
    $dataBytes = 0
    if (Test-Path $DataDir) { $dataBytes = (Get-ChildItem $DataDir -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum }
    $created = @()
    if (Test-Path $LedgerPath) {
        $created = @(Get-Content -Encoding UTF8 $LedgerPath | Where-Object { $_ } | Select-Object -Unique |
                     Where-Object { Test-Path (Join-Path $Project $_) })
    }
    return [pscustomobject]@{ Runs = $runs; DataBytes = [long]$dataBytes; Created = $created }
}

function Read-DataChoice($Inventory) {
    if ($Rest -contains '--keep-data') { return 'keep' }
    if ($Rest -contains '--delete-data') { return 'delete' }
    if ($Inventory.Runs.Count -eq 0 -and $Inventory.Created.Count -eq 0) { return 'delete' }
    $interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected
    if (-not $interactive) {
        Say 'No choice given for the run history (--keep-data or --delete-data); keeping it to be safe.'
        return 'keep'
    }
    while ($true) {
        $answer = Read-Host '[dwo-lens] Keep the run history and logs? [K]eep (moved out of the project) / [D]elete'
        if ($answer -match '^[kK]') { return 'keep' }
        if ($answer -match '^[dD]') { return 'delete' }
    }
}

function Invoke-Uninstall {
    $state = Read-State
    if (-not $state -and -not (Test-Path $LensDir) -and -not (Test-Path $PluginDest) -and -not (Test-Path $SmDest)) {
        Say 'dwo-Lens is not attached to this project.'
        Clear-ExcludeBlock | Out-Null
        if ($env:DWO_LENS_FROM_BAT -ne '1' -and (Test-Path $LauncherPath)) { Remove-Item -Force $LauncherPath }
        return
    }
    $inv = Get-UninstallInventory
    Say 'uninstall: removes everything dwo-Lens added to this project'
    Write-Host "    plugins     : $(@(@($PluginDest, $SmDest) | Where-Object { Test-Path $_ } | ForEach-Object { Split-Path -Leaf $_ }) -join ', ')"
    Write-Host "    launcher    : __dwo-lens.bat"
    Write-Host "    exclude     : $(if (Test-BlockPresent) { (Get-Vcs).Exclude } else { 'none' })"
    Write-Host ("    run history : {0} runs, {1:N1} MB (__dwo-lens\data)" -f $inv.Runs.Count, ($inv.DataBytes / 1MB))
    Write-Host "    other files : $($inv.Created.Count) (perf CSV captures in Saved\Profiling)"

    $choice = Read-DataChoice $inv
    if (-not (Remove-StateMonitor ($Rest -contains '--force'))) { Fail 'Uninstall stopped. StateMonitor is still in use.' }
    Remove-Plugin

    if ($choice -eq 'keep') {
        $dest = Join-Path $env:LOCALAPPDATA ("dwo-Lens\uninstalled\" + (Split-Path -Leaf $Project) + '-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Force $dest | Out-Null
        if (Test-Path $DataDir) { Move-Item $DataDir (Join-Path $dest 'data') }
        foreach ($f in $inv.Created) {
            $target = Join-Path $dest ("project-files\" + $f)
            New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
            Move-Item (Join-Path $Project $f) $target
        }
        Say "Run history kept at $dest"
    } else {
        foreach ($f in $inv.Created) { Remove-Item -Force (Join-Path $Project $f) -ErrorAction SilentlyContinue }
        Say "Run history and $($inv.Created.Count) other file(s) deleted."
    }
    # Engine folders dwo-Lens runs created and that are now empty (for example Saved\Profiling\CSV).
    foreach ($f in $inv.Created) {
        $dir = Split-Path -Parent (Join-Path $Project $f)
        while ($dir -and $dir.Length -gt $Project.Length -and (Test-Path $dir) -and -not (Get-ChildItem -Force $dir)) {
            Remove-Item -Force $dir
            $dir = Split-Path -Parent $dir
        }
    }

    $cleared = Clear-ExcludeBlock
    Set-Location $Project
    if (Test-Path $LensDir) {
        Get-ChildItem $LensDir -Recurse -Force -File | ForEach-Object { if ($_.IsReadOnly) { $_.IsReadOnly = $false } }
        Remove-Item -Recurse -Force $LensDir
    }
    # A launcher run by itself deletes its own file after this script returns.
    if ($env:DWO_LENS_FROM_BAT -ne '1' -and (Test-Path $LauncherPath)) { Remove-Item -Force $LauncherPath }

    $left = @($PluginDest, $SmDest, $LensDir) | Where-Object { Test-Path $_ }
    if ($env:DWO_LENS_FROM_BAT -ne '1' -and (Test-Path $LauncherPath)) { $left += $LauncherPath }
    if ($left.Count -gt 0) { Say "Uninstalled, but these remain: $($left -join ', ')"; exit 3 }
    if ($cleared) { Say 'Exclude entries removed.' }
    $vcs = Get-Vcs
    if ($vcs.Kind -ne 'none' -and $state -and $state.gitStatusBeforeAttach -ne $null) {
        $after = (@((Get-ProjectStatus) -split "`n") | Where-Object { $_ -and $_ -notmatch '__dwo-lens\.bat$' }) -join "`n"
        if ($after -ne $state.gitStatusBeforeAttach) { Say 'Uninstalled, but git status differs from before attach. Review: git status'; exit 3 }
        Say 'Uninstalled. git status matches the state before attach.'
    } else {
        Say 'Uninstalled. No dwo-Lens files remain in the project.'
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
    'smoke'   { Invoke-Smoke }
    'perf'    { Invoke-Perf }
    'sizes'   { Invoke-Sizes }
    'trace'   { Invoke-Trace }
    'play'    { Invoke-Play }
    'tools'   { Invoke-Tools }
    'sm-install' { Invoke-SmInstall }
    'sm-remove'  { Invoke-SmRemove }
    'gui'     { & (Join-Path $SystemDir 'tools\gui.ps1') -SystemDir $SystemDir -Project $Project -LensHome $SystemDir }
    'uninstall' { Invoke-Uninstall }
    'detach'  { Invoke-Uninstall }
}

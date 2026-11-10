# dwo-Forge control script. Runs from the dwo-Forge install folder (a git clone or an extracted zip);
# invoked by dwo-Forge.bat, the GUI, or the project's __dwo-forge.bat launcher.
# Windows PowerShell 5.1 compatible. Never modifies tracked project files.
#
# dwo-Forge offers three tools for an Unreal project:
#   Lens            analysis lines (analyze, verify, logs, smoke, perf, sizes, trace) + DwoLensInspector commandlet
#   DwoStateMonitor live runtime state viewer plugin (sm-install, sm-remove)
#   DwoCanvas       design file -> UMG Widget Blueprint importer (canvas-install, canvas-remove, canvas-import, canvas-validate)
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('attach', 'update', 'status', 'analyze', 'verify', 'logs', 'smoke', 'perf', 'sizes', 'trace', 'play', 'tools', 'gui',
                 'sm-install', 'sm-remove', 'canvas-install', 'canvas-remove', 'canvas-import', 'canvas-validate',
                 'doctor', 'uninstall', 'detach')]
    [string]$Command,
    [Parameter(Mandatory = $true)]
    [string]$Project,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Rest
)

$ErrorActionPreference = 'Stop'
$Utf8 = New-Object System.Text.UTF8Encoding($false)

$SystemDir   = Split-Path -Parent $PSScriptRoot
$Project     = (Resolve-Path $Project).Path.TrimEnd('')
$ForgeDir    = Join-Path $Project '__dwo-forge'
$DataDir     = Join-Path $ForgeDir 'data'
$StatePath   = Join-Path $ForgeDir 'state.json'
$PluginDest  = Join-Path $Project 'Plugins\__DwoLensInspector'
$Marker      = '.dwo-forge-managed'
$Markers     = @('.dwo-forge-managed', '.dwo-lens-managed')   # dwo-Lens 1.x wrote the second one
$BlockEnd    = '# <<< dwo-forge managed <<<'
$SmDest      = Join-Path $Project 'Plugins\__DwoStateMonitor'
$SmLegacyDest = Join-Path $Project 'Plugins\__StateMonitor'      # dwo-Lens 1.x install location
$CanvasDest  = Join-Path $Project 'Plugins\__DwoCanvas'
$CanvasOldDest = Join-Path $Project 'Plugins\DwoCanvas'           # 2.0.0 installed it here, visible to git
$CanvasOldMarker = '.dwo-forge-installed'                        # ... with this marker
# Legacy names stay listed so leftovers of a migration in progress never show up in git.
$ManagedPaths = @('__dwo-forge/', '__dwo-forge.bat', 'Plugins/__DwoLensInspector/', 'Plugins/__DwoStateMonitor/',
                  'Plugins/__DwoCanvas/', '__dwo-lens/', '__dwo-lens.bat', 'Plugins/__StateMonitor/')
$LauncherPath = Join-Path $Project '__dwo-forge.bat'
$LedgerPath  = Join-Path $ForgeDir 'created-files.txt'   # files dwo-Forge caused outside __dwo-forge (removed by uninstall)
$AppDataDir  = Join-Path $env:LOCALAPPDATA 'dwo-Forge'
# dwo-Lens 1.x names, migrated on the first command run against the project.
$LegacyDir      = Join-Path $Project '__dwo-lens'
$LegacyLauncher = Join-Path $Project '__dwo-lens.bat'
$LegacyAppData  = Join-Path $env:LOCALAPPDATA 'dwo-Lens'
$FromLegacyBat  = ($env:DWO_LENS_FROM_BAT -eq '1') -and ($env:DWO_FORGE_FROM_BAT -ne '1')

function Say([string]$Message) { Write-Host "[dwo-forge] $Message" }
function Fail([string]$Message) { Write-Host "[dwo-forge] ERROR: $Message" -ForegroundColor Red; exit 1 }

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
    $block = @("# >>> dwo-forge managed: $id (do not edit) >>>") + @($ManagedPaths | ForEach-Object { '/' + $vcs.Prefix + $_ }) + @($BlockEnd)
    $all = @($kept | Where-Object { $_ -ne $null }) + @('') + $block
    $dir = Split-Path -Parent $vcs.Exclude
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
    [IO.File]::WriteAllText($vcs.Exclude, (($all -join "`n").Trim() + "`n"), $Utf8)
    return $vcs
}

# Removes this project's block, whether written by dwo-Forge or by dwo-Lens 1.x ('# >>> dwo-lens managed: <id> ...').
# The unnamed block of the oldest versions belongs to the repository root project.
function Remove-Block([string[]]$Lines, [string]$Id) {
    $out = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in $Lines) {
        if ($line -like '# >>> dwo-forge managed*' -or $line -like '# >>> dwo-lens managed*') {
            $legacy = $line -notlike '# >>> dwo-* managed: *'
            $mine = ($legacy -and $Id -eq '.') -or $line -like "# >>> dwo-forge managed: $Id *" -or $line -like "# >>> dwo-lens managed: $Id *"
            if ($mine) { $inside = $true; continue }
        }
        if ($inside) {
            if ($line -like '# <<< dwo-forge managed*' -or $line -like '# <<< dwo-lens managed*') { $inside = $false }
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

function Test-BlockPresent([switch]$Legacy) {
    # Default: the current dwo-Forge block. -Legacy: a block written by dwo-Lens 1.x for this project.
    $vcs = Get-Vcs
    if ($vcs.Kind -eq 'none' -or -not $vcs.Exclude -or -not (Test-Path $vcs.Exclude)) { return $false }
    $id = Get-BlockId $vcs
    $text = Get-Content -Raw $vcs.Exclude
    if (-not $text) { return $false }
    if ($Legacy) { return ($text -like "*# >>> dwo-lens managed: $id *") -or ($id -eq '.' -and $text -like '*# >>> dwo-lens managed (do not edit)*') }
    return ($text -like "*# >>> dwo-forge managed: $id *")
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

# ---------------------------------------------------------------- plugin builds (shared by every provided tool)
# Build products a developer may have next to the plugin source; never part of the hash or the build input.
$PluginBuildDirs = @('Binaries', 'Intermediate', 'Saved', '__pycache__')

function Get-PluginSourceFiles([string]$Src) {
    return Get-ChildItem -Recurse -File $Src | Where-Object {
        $rel = $_.FullName.Substring($Src.Length + 1)
        -not ($PluginBuildDirs | Where-Object { $rel -like "$_\*" -or $rel -like "*\$_\*" })
    }
}

function Get-PluginSourceHash([string]$Name = 'DwoLensInspector') {
    $src = Join-Path $SystemDir "plugin\$Name"
    $sha = [Security.Cryptography.SHA256]::Create()
    $sb = New-Object System.Text.StringBuilder
    Get-PluginSourceFiles $src | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($src.Length)
        $h = (Get-FileHash -Algorithm SHA256 $_.FullName).Hash
        [void]$sb.Append("$rel=$h;")
    }
    $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($sb.ToString()))
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 12)
}

# Builds plugin\<Name> with BuildPlugin once per engine version and source hash, cached under
# %LOCALAPPDATA%\dwo-Forge\plugin-cache. $Module is the editor module whose DLL proves the build worked.
function Get-PluginBuild($Engine, [string]$Name = 'DwoLensInspector', [string]$Module = '') {
    if (-not $Module) { $Module = $Name }
    $hash = Get-PluginSourceHash $Name
    $cacheRoot = Join-Path $AppDataDir 'plugin-cache'
    $cache = Join-Path $cacheRoot "$($Engine.Version)-$Name-$hash"
    $dll = Join-Path $cache "Binaries\Win64\UnrealEditor-$Module.dll"
    if (Test-Path $dll) { return [pscustomobject]@{ Dir = $cache; Hash = $hash } }

    Say "Building $Name for UE $($Engine.Version) (first time for this engine/plugin version, takes a few minutes)..."
    $staging = Join-Path $cacheRoot "staging-$Name-$hash"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    $logDir = if (Test-Path $ForgeDir) { Join-Path $DataDir 'logs' } else { Join-Path $cacheRoot 'logs' }
    $log = Join-Path $logDir "build-$Name.log"
    New-Item -ItemType Directory -Force $logDir | Out-Null
    # Build from a copy: BuildPlugin writes files (Config/FilterPlugin.ini) into the plugin folder,
    # and the install folder must stay as shipped.
    $src = Join-Path $SystemDir "plugin\$Name"
    $srcCopy = Join-Path $cacheRoot "src-$Name-$hash"
    if (Test-Path $srcCopy) { Remove-Item -Recurse -Force $srcCopy }
    foreach ($f in (Get-PluginSourceFiles $src)) {
        $target = Join-Path $srcCopy $f.FullName.Substring($src.Length + 1)
        New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
        Copy-Item $f.FullName $target
    }
    $uplugin = Join-Path $srcCopy "$Name.uplugin"
    & cmd /c "`"$($Engine.RunUAT)`" BuildPlugin -Plugin=`"$uplugin`" -Package=`"$staging`" -HostPlatforms=Win64 -TargetPlatforms=Win64 > `"$log`" 2>&1"
    $code = $LASTEXITCODE
    Remove-Item -Recurse -Force $srcCopy -ErrorAction SilentlyContinue
    if ($code -ne 0 -or -not (Test-Path (Join-Path $staging "Binaries\Win64\UnrealEditor-$Module.dll"))) {
        if (Test-Path $log) { Select-String -Path $log -Pattern ': (fatal )?error ' | Select-Object -First 5 | ForEach-Object { Write-Host "    $($_.Line.Trim())" -ForegroundColor Red } }
        Remove-Item -Recurse -Force $staging -ErrorAction SilentlyContinue
        Fail "Plugin build failed. See $log"
    }
    if (Test-Path $cache) { Remove-Item -Recurse -Force $cache }
    Move-Item $staging $cache
    return [pscustomobject]@{ Dir = $cache; Hash = $hash }
}

function Copy-PluginExtras([string]$Name, [string]$Dest) {
    # BuildPlugin packages only Binaries, Source, Content, Resources and what FilterPlugin.ini lists. Everything else
    # the plugin ships next to them is needed at run time too: Config\Engine.ini carries [CoreRedirects],
    # DwoCanvas reads Schema\*.json while importing, and README/Samples belong with the plugin. Copy whatever the
    # build output lacks from the shipped source (never overwriting built files).
    $src = Join-Path $SystemDir "plugin\$Name"
    foreach ($item in (Get-ChildItem -Force $src)) {
        if ($PluginBuildDirs -contains $item.Name) { continue }
        $target = Join-Path $Dest $item.Name
        if (-not $item.PSIsContainer) { if (-not (Test-Path $target)) { Copy-Item $item.FullName $target }; continue }
        foreach ($f in (Get-PluginSourceFiles $item.FullName)) {
            $t = Join-Path $target $f.FullName.Substring($item.FullName.Length + 1)
            if (Test-Path $t) { continue }
            New-Item -ItemType Directory -Force (Split-Path -Parent $t) | Out-Null
            Copy-Item $f.FullName $t
        }
    }
}

function Read-SharedText([string]$Path) {
    # Reads a log that a lingering engine child process may still hold open for writing.
    $share = [IO.FileShare]([int][IO.FileShare]::ReadWrite -bor [int][IO.FileShare]::Delete)
    $fs = New-Object IO.FileStream -ArgumentList $Path, ([IO.FileMode]::Open), ([IO.FileAccess]::Read), $share
    try { $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8); return $sr.ReadToEnd() } finally { $fs.Dispose() }
}

function Test-Managed([string]$Dir) {
    # A plugin folder dwo-Forge (or dwo-Lens 1.x) installed: it carries one of the marker files.
    foreach ($m in $Markers) { if (Test-Path (Join-Path $Dir $m)) { return $true } }
    return $false
}

function Install-Plugin($Build) {
    Remove-Plugin
    New-Item -ItemType Directory -Force $PluginDest | Out-Null
    # Prebuilt binaries only: the project's own build never compiles this plugin.
    Copy-Item (Join-Path $Build.Dir 'DwoLensInspector.uplugin') $PluginDest
    # Disabled by default: the editor, cooks and packages never load or stage it. dwo-Forge enables it only
    # for its own commandlet runs (-EnablePlugins=DwoLensInspector), so the project needs no .uproject entry.
    $upluginPath = Join-Path $PluginDest 'DwoLensInspector.uplugin'
    $uplugin = Get-Content -Raw -Encoding UTF8 $upluginPath | ConvertFrom-Json
    $uplugin | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $false -Force
    [IO.File]::WriteAllText($upluginPath, ($uplugin | ConvertTo-Json -Depth 10), $Utf8)
    Copy-Item -Recurse (Join-Path $Build.Dir 'Binaries') $PluginDest
    if (Test-Path (Join-Path $Build.Dir 'Resources')) { Copy-Item -Recurse (Join-Path $Build.Dir 'Resources') $PluginDest }
    [IO.File]::WriteAllText((Join-Path $PluginDest $Marker), "Managed by dwo-Forge (Lens inspector). Removed by '__dwo-forge.bat uninstall'.`n", $Utf8)
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
    if (-not (Test-Managed $PluginDest)) {
        Fail "$PluginDest exists but is not managed by dwo-Forge. Leaving it untouched."
    }
    Remove-Folder $PluginDest 'Plugins\__DwoLensInspector'
    Remove-EmptyPluginsFolder
}

function Remove-Folder([string]$Path, [string]$What) {
    # Removes a folder; a file held open (an editor using a plugin DLL) stops the command with a clear message.
    try { Remove-Item -Recurse -Force $Path -ErrorAction Stop }
    catch { Fail "Cannot remove $What ($Path): $($_.Exception.Message) Close the Unreal Editor and try again." }
}

# ---------------------------------------------------------------- DwoStateMonitor (provided tool)
function Get-StateMonitorReferences {
    # Project code that includes or calls the state monitor. Removing the plugin would break those builds.
    $refs = @()
    foreach ($dir in @('Source', 'Plugins')) {
        $path = Join-Path $Project $dir
        if (-not (Test-Path $path)) { continue }
        $refs += Get-ChildItem -Path $path -Recurse -File -Include *.h, *.cpp, *.cs -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notlike '*\Plugins\__*' } |
                 Select-String -Pattern 'StateMonitor' -List | ForEach-Object { $_.Path.Substring($Project.Length + 1) + ':' + $_.LineNumber }
    }
    return $refs
}

function Get-StaleStateMonitorDeps {
    # Places that still name the module/plugin 'StateMonitor' (dwo-Lens 1.x). With DwoStateMonitor they must say
    # 'DwoStateMonitor'; class names and headers did not change, so nothing else in the code needs editing.
    $hits = @()
    foreach ($dir in @('Source', 'Plugins')) {
        $path = Join-Path $Project $dir
        if (-not (Test-Path $path)) { continue }
        $hits += Get-ChildItem -Path $path -Recurse -File -Filter *.Build.cs -ErrorAction SilentlyContinue |
                 Where-Object { $_.FullName -notlike '*\Plugins\__*' -and $_.Name -ne 'StateMonitor.Build.cs' } |
                 Select-String -Pattern '"StateMonitor"' | ForEach-Object { $_.Path.Substring($Project.Length + 1) + ':' + $_.LineNumber + '  (Build.cs dependency)' }
    }
    $files = @(Get-ChildItem -Path $Project -Filter *.uproject -File)
    if ($files.Count -eq 1) {
        $json = Get-Content -Raw -Encoding UTF8 $files[0].FullName | ConvertFrom-Json
        if (@($json.Plugins | Where-Object { $_.Name -eq 'StateMonitor' }).Count -gt 0) { $hits += "$($files[0].Name)  (Plugins entry)" }
    }
    return $hits
}

function Write-StaleDepsWarning {
    # Returns $true when something still names the old module.
    if (Find-OwnStateMonitor) { return $false }   # the project ships its own plugin; its names are its business
    $stale = @(Get-StaleStateMonitorDeps)
    if ($stale.Count -eq 0) { return $false }
    Write-Host '[dwo-forge] WARNING: the plugin is now named DwoStateMonitor, but the project still refers to "StateMonitor":' -ForegroundColor Yellow
    $stale | Select-Object -First 15 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
    Say '  Replace "StateMonitor" with "DwoStateMonitor" in those Build.cs dependency lists / .uproject Plugins entries.'
    Say '  Class names (UStateMonitorSubsystem, ...) and includes stay the same; saved Blueprints load through CoreRedirects.'
    return $true
}

function Find-OwnStateMonitor {
    # A state monitor that belongs to the project itself (not installed by dwo-Forge/dwo-Lens).
    $plugins = Join-Path $Project 'Plugins'
    if (-not (Test-Path $plugins)) { return $null }
    return Get-ChildItem -Path $plugins -Recurse -Include 'DwoStateMonitor.uplugin', 'StateMonitor.uplugin' -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notlike '*\Plugins\__*' } | Select-Object -First 1
}

function Get-InstalledStateMonitor {
    # The managed install: DwoStateMonitor (current) or __StateMonitor (dwo-Lens 1.x, migrated by update/sm-install).
    if (Test-Path $SmDest) { return [pscustomobject]@{ Dir = $SmDest; Legacy = $false; Uplugin = (Join-Path $SmDest 'DwoStateMonitor.uplugin') } }
    if (Test-Path $SmLegacyDest) { return [pscustomobject]@{ Dir = $SmLegacyDest; Legacy = $true; Uplugin = (Join-Path $SmLegacyDest 'StateMonitor.uplugin') } }
    return $null
}

function Test-RuntimeMode([string]$UpluginPath) {
    if (-not (Test-Path $UpluginPath)) { return $false }
    return -not ((Get-Content -Raw -Encoding UTF8 $UpluginPath) -match 'TargetAllowList')
}

function Install-StateMonitor($Engine, [bool]$Runtime) {
    $build = Get-PluginBuild $Engine 'DwoStateMonitor'
    if (Test-Path $SmDest) { Remove-Folder $SmDest 'Plugins\__DwoStateMonitor' }
    New-Item -ItemType Directory -Force $SmDest | Out-Null
    # Installed as a source plugin with prebuilt editor binaries: the editor opens without compiling, and
    # packaging (including Blueprint-only projects) builds a game target that contains DwoStateMonitor.
    Get-ChildItem $build.Dir | Where-Object { $_.Name -ne 'Intermediate' } | ForEach-Object { Copy-Item -Recurse $_.FullName $SmDest }
    Copy-PluginExtras 'DwoStateMonitor' $SmDest   # Config\Engine.ini: CoreRedirects /Script/StateMonitor -> /Script/DwoStateMonitor
    $upluginPath = Join-Path $SmDest 'DwoStateMonitor.uplugin'
    $uplugin = Get-Content -Raw -Encoding UTF8 $upluginPath | ConvertFrom-Json
    $uplugin | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $true -Force
    $uplugin | Add-Member -NotePropertyName Installed -NotePropertyValue $false -Force
    if (-not $Runtime) {
        # Default: editor and PIE only. Packaged games never load the module, so installing it cannot
        # change or break a package. (A Blueprint-only package uses the stock UnrealGame executable,
        # which cannot contain a plugin the .uproject does not list.)
        foreach ($m in $uplugin.Modules) { $m | Add-Member -NotePropertyName TargetAllowList -NotePropertyValue @('Editor') -Force }
    }
    [IO.File]::WriteAllText($upluginPath, ($uplugin | ConvertTo-Json -Depth 10), $Utf8)
    [IO.File]::WriteAllText((Join-Path $SmDest $Marker), "DwoStateMonitor installed by dwo-Forge. Remove with '__dwo-forge.bat sm-remove'.`n", $Utf8)
}

function Move-LegacyStateMonitor($Engine) {
    # dwo-Lens 1.x installed the plugin as Plugins\__StateMonitor (module StateMonitor). Replace it with
    # Plugins\__DwoStateMonitor in the same mode. Saved assets resolve through the plugin's CoreRedirects.
    if (-not (Test-Path $SmLegacyDest)) { return $false }
    if (-not (Test-Managed $SmLegacyDest)) { Say 'Plugins\__StateMonitor is not managed by dwo-Forge; leaving it.'; return $false }
    $runtime = Test-RuntimeMode (Join-Path $SmLegacyDest 'StateMonitor.uplugin')
    Say "Migrating Plugins\__StateMonitor (dwo-Lens 1.x) to Plugins\__DwoStateMonitor ($(if ($runtime) { 'runtime' } else { 'editor and PIE' }))..."
    Install-StateMonitor $Engine $runtime
    Remove-Folder $SmLegacyDest 'Plugins\__StateMonitor'
    Say 'StateMonitor migrated to DwoStateMonitor.'
    $null = Write-StaleDepsWarning
    return $true
}

function Invoke-SmInstall {
    if (-not (Read-State)) { Fail 'Not attached. Run attach first.' }
    $own = Find-OwnStateMonitor
    if ($own) { Fail "The project already contains a state monitor plugin ($($own.FullName.Substring($Project.Length + 1))). Nothing to install." }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $null = Set-ExcludeBlock
    $runtime = $Rest -contains '--runtime'
    $legacy = (Test-Path $SmLegacyDest) -and (Test-Managed $SmLegacyDest)
    if ($legacy -and -not $runtime) { $runtime = Test-RuntimeMode (Join-Path $SmLegacyDest 'StateMonitor.uplugin') }
    Install-StateMonitor $engine $runtime
    if ($legacy) { Remove-Folder $SmLegacyDest 'Plugins\__StateMonitor'; Say 'Replaced the dwo-Lens 1.x install (Plugins\__StateMonitor).' }
    if ($runtime) {
        Say "DwoStateMonitor installed for runtime: Plugins\__DwoStateMonitor (UE $($engine.Version))"
        Say 'Packaged games include it only when the project declares it:'
        if ($uproject.IsCode) { Say '  C++ project: add "DwoStateMonitor" to the Build.cs of the module that calls it.' }
        else { Say '  Blueprint-only project: add {"Name": "DwoStateMonitor", "Enabled": true} to Plugins in the .uproject.' }
        Say 'Remove those entries before sm-remove or uninstall.'
    } else {
        Say "DwoStateMonitor installed for editor and PIE: Plugins\__DwoStateMonitor (UE $($engine.Version))"
        Say 'Packaged builds are not affected. For packaged games use: __dwo-forge.bat sm-install --runtime'
    }
    Say 'Live view: open dwo-Forge > DwoStateMonitor, then Play in the editor or run __dwo-forge.bat play. No project code needed.'
    Say 'Optional: Blueprint Watch State Object / Submit State Snapshot. C++: UStateMonitorSubsystem::Get(this)->Submit(TEXT("Id"), State).'
    $null = Write-StaleDepsWarning
}

function Remove-StateMonitor([bool]$Force) {
    $installed = @(@($SmDest, $SmLegacyDest) | Where-Object { Test-Path $_ })
    if ($installed.Count -eq 0) { return $true }
    foreach ($dir in $installed) { if (-not (Test-Managed $dir)) { Fail "$dir is not managed by dwo-Forge. Leaving it untouched." } }
    $refs = @(Get-StateMonitorReferences)
    if ($refs.Count -gt 0 -and -not $Force) {
        Say 'Project code still uses the state monitor. Removing the plugin would break the build:'
        $refs | Select-Object -First 15 | ForEach-Object { Write-Host "    $_" }
        Say 'Remove those references first, or run again with --force.'
        return $false
    }
    foreach ($dir in $installed) { Remove-Folder $dir (Split-Path -Leaf $dir) }
    Remove-EmptyPluginsFolder
    Say 'DwoStateMonitor removed.'
    return $true
}

function Invoke-SmRemove {
    if (-not (Get-InstalledStateMonitor)) { Say 'DwoStateMonitor is not installed by dwo-Forge.'; return }
    if (-not (Remove-StateMonitor ($Rest -contains '--force'))) { exit 1 }
}

# ---------------------------------------------------------------- DwoCanvas (provided tool)
# Managed like DwoStateMonitor: the source stays in dwo-Forge, canvas-install copies it to Plugins\__DwoCanvas
# (git-excluded), and canvas-remove / uninstall delete it. Widget Blueprints made by canvas-import use the
# DwoCanvasRuntime module, so removal is refused while such assets exist (unless --force).
function Get-CanvasState {
    # none | managed (installed by dwo-Forge) | own (the project's own copy)
    if (Test-Path (Join-Path $CanvasDest 'DwoCanvas.uplugin')) {
        if (Test-Managed $CanvasDest) { return 'managed' }
        return 'own'
    }
    if (Test-Path (Join-Path $CanvasOldDest $CanvasOldMarker)) { return 'managed' }   # 2.0.0 install, not moved yet
    $plugins = Join-Path $Project 'Plugins'
    if ((Test-Path $plugins) -and (Get-ChildItem -Path $plugins -Recurse -Filter 'DwoCanvas.uplugin' -ErrorAction SilentlyContinue |
                                   Where-Object { $_.FullName -notlike '*\Plugins\__*' } | Select-Object -First 1)) { return 'own' }
    return 'none'
}

function Get-CanvasUsers {
    # Assets that reference DwoCanvasRuntime (classes of widgets generated by canvas-import).
    $content = Join-Path $Project 'Content'
    if (-not (Test-Path $content)) { return @() }
    $users = New-Object System.Collections.Generic.List[string]
    $latin1 = [Text.Encoding]::GetEncoding(28591)   # one char per byte
    foreach ($f in (Get-ChildItem -Path $content -Recurse -File -Include *.uasset, *.umap -ErrorAction SilentlyContinue)) {
        # Imported class and package names live in the package header (name and import tables) at the start
        # of the file; the first 8 MB cover it for any realistic widget, and the bulk data after it never matters.
        $stream = [IO.File]::OpenRead($f.FullName)
        try {
            $buffer = New-Object byte[] ([Math]::Min($stream.Length, 8MB))
            $read = $stream.Read($buffer, 0, $buffer.Length)
        } finally { $stream.Dispose() }
        if ($latin1.GetString($buffer, 0, $read).Contains('DwoCanvasRuntime')) { $users.Add($f.FullName.Substring($Project.Length + 1)) }
    }
    return $users.ToArray()
}

function Invoke-CanvasInstall {
    if (-not (Read-State)) { Fail 'Not attached. Run attach first.' }
    $state = Get-CanvasState
    if ($state -eq 'own') { Fail 'The project already contains its own DwoCanvas plugin. Nothing to install.' }
    $src = Join-Path $SystemDir 'plugin\DwoCanvas\DwoCanvas.uplugin'
    if (-not (Test-Path $src)) { Fail "DwoCanvas is not part of this dwo-Forge install ($src missing)." }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    # Prebuilt editor binaries, like DwoStateMonitor: Blueprint-only projects open without compiling.
    $build = Get-PluginBuild $engine 'DwoCanvas' 'DwoCanvasEditor'
    $null = Set-ExcludeBlock
    if (Test-Path $CanvasDest) { Remove-Folder $CanvasDest 'Plugins\__DwoCanvas' }
    if (Test-Path (Join-Path $CanvasOldDest $CanvasOldMarker)) { Remove-Folder $CanvasOldDest 'Plugins\DwoCanvas (2.0.0 install)' }
    New-Item -ItemType Directory -Force $CanvasDest | Out-Null
    Get-ChildItem $build.Dir | Where-Object { $_.Name -ne 'Intermediate' } | ForEach-Object { Copy-Item -Recurse $_.FullName $CanvasDest }
    Copy-PluginExtras 'DwoCanvas' $CanvasDest     # Schema\, Config\, Samples\, README
    $upluginPath = Join-Path $CanvasDest 'DwoCanvas.uplugin'
    $uplugin = Get-Content -Raw -Encoding UTF8 $upluginPath | ConvertFrom-Json
    $uplugin | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $true -Force
    $uplugin | Add-Member -NotePropertyName Installed -NotePropertyValue $false -Force
    [IO.File]::WriteAllText($upluginPath, ($uplugin | ConvertTo-Json -Depth 10), $Utf8)
    [IO.File]::WriteAllText((Join-Path $CanvasDest $Marker),
        "DwoCanvas $($uplugin.VersionName) installed by dwo-Forge ($(Get-SystemCommit)). Removed by '__dwo-forge.bat canvas-remove' or uninstall.`n", $Utf8)
    Say "DwoCanvas installed: Plugins\__DwoCanvas (UE $($engine.Version)), hidden from git like the other dwo-Forge plugins."
    if (-not $uproject.IsCode) { Say 'Blueprint-only project: prebuilt editor binaries are included, so the editor opens without compiling.' }
    Say 'Widget Blueprints made by canvas-import use DwoCanvasRuntime: anyone opening them needs DwoCanvas installed too.'
    Say 'Next: __dwo-forge.bat canvas-validate <file.dwocanvas.json>, then canvas-import <file>.'
}

function Move-OldCanvasInstall {
    # 2.0.0 installed DwoCanvas visibly as Plugins\DwoCanvas; move it to the managed, git-excluded location.
    if (-not (Test-Path (Join-Path $CanvasOldDest $CanvasOldMarker))) { return }
    if (Test-Path $CanvasDest) { return }
    try { Move-Item $CanvasOldDest $CanvasDest -ErrorAction Stop }
    catch { Say "Plugins\DwoCanvas (2.0.0 install) could not be moved to Plugins\__DwoCanvas: $($_.Exception.Message) Close the editor and run any command again."; return }
    Remove-Item -Force (Join-Path $CanvasDest $CanvasOldMarker) -ErrorAction SilentlyContinue
    [IO.File]::WriteAllText((Join-Path $CanvasDest $Marker), "DwoCanvas installed by dwo-Forge. Removed by '__dwo-forge.bat canvas-remove' or uninstall.`n", $Utf8)
    $null = Set-ExcludeBlock
    Say 'Moved Plugins\DwoCanvas to Plugins\__DwoCanvas (managed by dwo-Forge, hidden from git).'
}

function Remove-Canvas([bool]$Force) {
    # $true when nothing managed is left. The project's own DwoCanvas is never touched.
    $dirs = @(@($CanvasDest, $CanvasOldDest) | Where-Object { (Test-Path $_) -and ((Test-Managed $_) -or (Test-Path (Join-Path $_ $CanvasOldMarker))) })
    if ($dirs.Count -eq 0) { return $true }
    $users = @(Get-CanvasUsers)
    if ($users.Count -gt 0 -and -not $Force) {
        Say "$($users.Count) asset(s) still use DwoCanvasRuntime (generated widgets). Removing the plugin would break them:"
        $users | Select-Object -First 15 | ForEach-Object { Write-Host "    $_" }
        Say 'Delete or rebuild those widgets first, or run again with --force.'
        return $false
    }
    foreach ($dir in $dirs) { Remove-Folder $dir (Split-Path -Leaf $dir) }
    Remove-EmptyPluginsFolder
    Say 'DwoCanvas removed. Import records (DwoCanvas\Records) and reports (Saved\DwoCanvas) were left in place.'
    return $true
}

function Invoke-CanvasRemove {
    $state = Get-CanvasState
    if ($state -eq 'none') { Say 'DwoCanvas is not installed.'; return }
    if ($state -eq 'own') { Fail 'DwoCanvas in this project was not installed by dwo-Forge. Leaving it untouched.' }
    if (-not (Remove-Canvas ($Rest -contains '--force'))) { exit 1 }
}

function Get-FileArg {
    # First argument that is not an option (and not the value of --doc).
    $skip = $false
    foreach ($a in $Rest) {
        if ($skip) { $skip = $false; continue }
        if (-not $a) { continue }
        if ($a -eq '--doc') { $skip = $true; continue }
        if ($a.StartsWith('--')) { continue }
        return $a
    }
    return $null
}

function Resolve-DesignFile {
    $file = Get-FileArg
    if (-not $file) { Fail "Usage: $Command <file.dwocanvas.json>" }
    foreach ($candidate in @($file, (Join-Path $Project $file))) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $candidate).Path }
    }
    Fail "Design file not found: $file"
}

function Get-CanvasReports([datetime]$Since) {
    $dir = Join-Path $Project 'Saved\DwoCanvas\Reports'
    if (-not (Test-Path $dir)) { return @() }
    return @(Get-ChildItem $dir -Filter *.txt -File | Where-Object { $_.LastWriteTime -ge $Since } | Sort-Object LastWriteTime -Descending)
}

function Invoke-CanvasImport {
    $file = Resolve-DesignFile
    if ((Get-CanvasState) -eq 'none') { Fail 'DwoCanvas is not installed in this project. Run canvas-install first.' }
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    $reset = ($Rest -contains '--reset') -or ($Rest -contains '--reset-overrides')
    $doc = $null
    $list = @($Rest)
    for ($k = 0; $k -lt $list.Count - 1; $k++) { if ($list[$k] -eq '--doc') { $doc = $list[$k + 1] } }
    $logDir = if (Test-Path $ForgeDir) { Join-Path $DataDir 'canvas' } else { Join-Path $Project 'Saved\DwoCanvas\Logs' }
    New-Item -ItemType Directory -Force $logDir | Out-Null
    $log = Join-Path $logDir ('import-' + (Get-Date).ToString('yyyyMMdd-HHmmss') + '.log')
    $arguments = "`"$($uproject.Path)`" -run=DwoCanvasImport -File=`"$file`""
    if ($reset) { $arguments += ' -ResetOverrides' }
    if ($doc) { $arguments += " -Doc=`"$doc`"" }
    Say "canvas-import $(Split-Path -Leaf $file)$(if ($reset) { ' (reset overrides)' })$(if ($doc) { " (document $doc)" })"
    Say 'Close the Unreal Editor first: the import saves Widget Blueprints an open editor may hold.'
    $since = (Get-Date).AddSeconds(-2)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & cmd /c "`"$($engine.Cmd)`" $arguments -unattended -nullrhi -nosplash -nopause -NoSound -stdout -FullStdOutLogOutput > `"$log`" 2>&1"
    $code = $LASTEXITCODE
    $report = $null
    if (Test-Path $log) {
        foreach ($line in ((Read-SharedText $log) -split "`r?`n")) { if ($line -match 'Report:\s*(.+\.txt)\s*$') { $report = $Matches[1].Trim() } }
    }
    # The commandlet prints the path relative to the engine's working folder (Engine\Binaries\Win64).
    if ($report -and -not [IO.Path]::IsPathRooted($report)) { $report = [IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $engine.Cmd) $report)) }
    if (-not $report -or -not (Test-Path -LiteralPath $report)) { $r = Get-CanvasReports $since | Select-Object -First 1; $report = if ($r) { $r.FullName } else { $null } }
    if ($report) {
        $report = (Resolve-Path -LiteralPath $report).Path
        $lines = [IO.File]::ReadAllLines($report, [Text.Encoding]::UTF8)
        $lines | Select-Object -First 2 | ForEach-Object { Say $_ }
        $lines | Where-Object { $_ -like 'ERROR*' } | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Red }
        $lines | Where-Object { $_ -like 'WARN*' } | Select-Object -First 10 | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
    }
    if (Test-Path $ForgeDir) {
        Write-Json (Join-Path $DataDir 'canvas\last.json') ([ordered]@{
            schema = 'forge.canvas/0.1'; file = $file; reset = [bool]$reset; doc = $doc; exitCode = $code; report = $report
            log = $log; seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1); finishedAt = (Get-Date).ToString('o') })
    }
    Say "canvas-import finished in $([math]::Round($sw.Elapsed.TotalSeconds)) s, exit code $code ($(if ($code -eq 0) { 'no errors' } else { 'errors' }))"
    if ($report) { Say "Report: $report" } else { Say "No report was written. Engine log: $log" }
    if ($code -ne 0) { exit $code }
}

function Invoke-CanvasValidate {
    $file = Resolve-DesignFile
    $pyRoot = Join-Path $SystemDir 'canvas\python'
    if (-not (Test-Path (Join-Path $pyRoot 'dwocanvas'))) { Fail "The DwoCanvas validator is not available in this install ($pyRoot\dwocanvas missing)." }
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) { Fail 'Python 3.10+ is required for canvas-validate.' }
    $previous = $env:PYTHONPATH
    $env:PYTHONPATH = if ($previous) { "$pyRoot;$previous" } else { $pyRoot }
    $pyArgs = @('-m', 'dwocanvas', 'validate', $file)
    if ($Rest -contains '--json') { $pyArgs += '--json' } else { Say "canvas-validate $(Split-Path -Leaf $file)" }
    $ErrorActionPreference = 'Continue'
    & python @pyArgs
    $code = $LASTEXITCODE
    $env:PYTHONPATH = $previous
    if ($code -ne 0) { exit $code }
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
    # Remembers a file dwo-Forge caused outside __dwo-forge so uninstall can offer to remove it.
    if (-not $Path -or -not (Test-Path $ForgeDir)) { return }
    [IO.File]::AppendAllText($LedgerPath, $Path.Substring($Project.Length + 1) + "`n", $Utf8)
}

function Install-Launcher {
    # The project's __dwo-forge.bat: a small launcher that finds this install folder (home.txt) and runs forge.ps1.
    $src = Join-Path $SystemDir 'tools\project-launcher.bat'
    $existing = if (Test-Path $LauncherPath) { [IO.File]::ReadAllBytes($LauncherPath) } else { $null }
    $new = [IO.File]::ReadAllBytes($src)
    if (-not $existing -or [Convert]::ToBase64String($existing) -ne [Convert]::ToBase64String($new)) {
        # A running __dwo-forge.bat is read line by line by cmd; replace it only after it finished.
        if ($env:DWO_FORGE_FROM_BAT -eq '1' -and $existing) { Copy-Item $src (Join-Path $ForgeDir 'launcher.next') -Force }
        else { Copy-Item $src $LauncherPath -Force }
    }
}

# ---------------------------------------------------------------- migration from dwo-Lens 1.x
function Merge-Folder([string]$From, [string]$To) {
    # Moves what is missing in $To; anything $To already has wins. Locked files are left for the next run.
    New-Item -ItemType Directory -Force $To | Out-Null
    foreach ($c in @(Get-ChildItem -Force $From -ErrorAction SilentlyContinue)) {
        if ($c.Name -eq 'launcher.next') { Remove-Item -Force $c.FullName -ErrorAction SilentlyContinue; continue }
        $target = Join-Path $To $c.Name
        try {
            if (-not (Test-Path $target)) { Move-Item $c.FullName $target -ErrorAction Stop }
            elseif ($c.PSIsContainer) { Merge-Folder $c.FullName $target }
            else { Remove-Item -Force $c.FullName -ErrorAction Stop }
        } catch { }
    }
    if (-not (Get-ChildItem -Force $From -ErrorAction SilentlyContinue)) { Remove-Item -Force -Recurse $From -ErrorAction SilentlyContinue }
}

function Move-LegacyAppData {
    # %LOCALAPPDATA%\dwo-Lens (plugin and GUI caches, GUI settings, kept run histories) -> %LOCALAPPDATA%\dwo-Forge
    # 2.0.0's GUI launcher could leave a FILE named dwo-Forge (a copied gui.json); it blocks every cache below.
    if (Test-Path $AppDataDir -PathType Leaf) { Remove-Item -Force $AppDataDir -ErrorAction SilentlyContinue }
    if (-not (Test-Path $LegacyAppData)) { return }
    $moved = $false
    if (-not (Test-Path $AppDataDir)) {
        try { Move-Item $LegacyAppData $AppDataDir -ErrorAction Stop; $moved = $true } catch { }
    }
    if (-not $moved) { Merge-Folder $LegacyAppData $AppDataDir }
    if (-not (Test-Path $LegacyAppData)) { Say 'Moved %LOCALAPPDATA%\dwo-Lens to %LOCALAPPDATA%\dwo-Forge.' }
}

function Test-OurLauncher([string]$Path) {
    if (-not (Test-Path $Path)) { return $false }
    $text = [IO.File]::ReadAllText($Path)
    return ($text -match 'dwo-Lens project launcher' -or $text -match 'dwo-Forge legacy launcher')
}

function Invoke-Migration {
    # Renames what dwo-Lens 1.x put into the project. Runs before every command, so a project attached with
    # dwo-Lens is migrated the first time any dwo-Forge command (or the GUI) touches it.
    Move-LegacyAppData
    Move-OldCanvasInstall
    $hadLegacy = (Test-Path $LegacyDir) -or (Test-OurLauncher $LegacyLauncher) -or (Test-BlockPresent -Legacy)
    if (-not $hadLegacy) { return }
    if (Test-Path $LegacyDir) {
        if (-not (Test-Path $ForgeDir)) {
            try { Move-Item $LegacyDir $ForgeDir -ErrorAction Stop }
            catch { Fail "Cannot rename __dwo-lens to __dwo-forge: $($_.Exception.Message) Close programs using files in it (an old dwo-Lens window) and try again." }
            Say 'Migrated __dwo-lens to __dwo-forge (run history, settings and attach state kept).'
        } else {
            Merge-Folder $LegacyDir $ForgeDir
        }
    }
    $attached = Test-Path $StatePath
    if ($attached) {
        [IO.File]::WriteAllText((Join-Path $ForgeDir 'home.txt'), $SystemDir, $Utf8)
        if (Test-BlockPresent -Legacy) { $null = Set-ExcludeBlock; Say 'Replaced the dwo-lens git exclude block with the dwo-forge block.' }
    }
    if (Test-OurLauncher $LegacyLauncher) {
        if ($FromLegacyBat) {
            # The old launcher is running (cmd reads it line by line), so it cannot be deleted now. It moves
            # __dwo-lens\launcher.next over itself when it finishes: hand it a forwarder to __dwo-forge.bat
            # that deletes itself on its next use.
            New-Item -ItemType Directory -Force $LegacyDir | Out-Null
            Copy-Item (Join-Path $SystemDir 'tools\legacy-project-launcher.bat') (Join-Path $LegacyDir 'launcher.next') -Force
        } else {
            Remove-Item -Force $LegacyLauncher -ErrorAction SilentlyContinue
        }
        if ($attached -and -not (Test-Path $LauncherPath)) { Copy-Item (Join-Path $SystemDir 'tools\project-launcher.bat') $LauncherPath -Force }
        Say 'Project launcher renamed: __dwo-lens.bat -> __dwo-forge.bat.'
    }
    if (Test-Path $SmLegacyDest) { Say 'Plugins\__StateMonitor (dwo-Lens 1.x) is still installed. Run __dwo-forge.bat update to migrate it to DwoStateMonitor.' }
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
        'own'    { Say "git repository: dwo-Forge files are hidden through .git\info\exclude (local only, never committed)." }
        'parent' { Say "The project is inside the git repository $($vcs.Top). dwo-Forge files are hidden there under /$($vcs.Prefix)." }
        'none'   { Say 'No version control: nothing to hide, so no exclude entry is written.' }
    }

    New-Item -ItemType Directory -Force $ForgeDir | Out-Null
    # Older versions kept a git clone of the system inside the project. The system now runs from the install folder.
    $legacy = Join-Path $ForgeDir 'system'
    if (Test-Path $legacy) {
        Get-ChildItem $legacy -Recurse -Force -File | ForEach-Object { if ($_.IsReadOnly) { $_.IsReadOnly = $false } }
        Remove-Item -Recurse -Force $legacy
        Say 'Removed the old system copy (__dwo-forge\system); the system now runs from the dwo-Forge folder.'
    }
    [IO.File]::WriteAllText((Join-Path $ForgeDir 'home.txt'), $SystemDir, $Utf8)
    Install-Launcher

    $build = Get-PluginBuild $engine
    Install-Plugin $build
    if ($Mode -eq 'update') { $null = Move-LegacyStateMonitor $engine }

    $state = [ordered]@{
        schema                = 'forge.state/0.3'
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
    Say "$Mode complete: $($uproject.Name) / UE $($engine.Version) / dwo-Forge $($state.systemCommit)"
    Say "Open the control panel: dwo-Forge.bat (or __dwo-forge.bat in the project). Remove everything: __dwo-forge.bat uninstall"
    Invoke-Doctor | Out-Null
}

function Get-ToolSummary {
    # One line per provided tool, shared by status and doctor.
    $sm = Get-InstalledStateMonitor
    $smText = if ($sm -and $sm.Legacy) { 'installed by dwo-Lens 1.x as Plugins\__StateMonitor (run update to migrate)' }
              elseif ($sm) { "installed (Plugins\__DwoStateMonitor, $(if (Test-RuntimeMode $sm.Uplugin) { 'runtime' } else { 'editor and PIE' }))" }
              elseif (Find-OwnStateMonitor) { 'part of the project itself' }
              else { 'not installed (optional: sm-install)' }
    $canvasText = switch (Get-CanvasState) {
        'managed' { 'installed (Plugins\__DwoCanvas)' }
        'own'     { 'part of the project itself' }
        default   { 'not installed (optional: canvas-install)' }
    }
    return [pscustomobject]@{ StateMonitor = $smText; Canvas = $canvasText }
}

function Invoke-Status {
    $state = Read-State
    if (-not $state) {
        Say 'Not attached.'
        Say "DwoCanvas      : $((Get-ToolSummary).Canvas)"
        return
    }
    $tools = Get-ToolSummary
    Say "Project        : $($state.project) ($($state.projectType))"
    Say "Engine         : UE $($state.engineVersion)"
    Say "dwo-Forge      : $(Get-SystemCommit) at $SystemDir"
    if ($state.systemCommit -ne (Get-SystemCommit)) { Say "Attached with $($state.systemCommit). Run update to refresh the plugins and launcher." }
    Say "Version control: $((Get-Vcs).Kind)"
    $uproject = Get-UProject
    $engine = Resolve-Engine $uproject.Json.EngineAssociation
    if ($engine.Version -ne $state.engineVersion) { Say "Engine changed to $($engine.Version). Run __dwo-forge.bat update." }
    Say "Attached at    : $($state.attachedAt)"
    Say "Lens           : $(if (Test-Path $PluginDest) { 'inspector installed (Plugins\__DwoLensInspector)' } else { 'inspector missing (run update)' })"
    Say "DwoStateMonitor: $($tools.StateMonitor)"
    Say "DwoCanvas      : $($tools.Canvas)"
    $null = Write-StaleDepsWarning
}

function Invoke-Doctor {
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
        Check 'no dwo-Lens 1.x exclude block left' (-not (Test-BlockPresent -Legacy)) 'run update'
        $leaks = Invoke-Git @('status', '--porcelain', '--untracked-files=all', '--', '__dwo-forge', '__dwo-forge.bat', 'Plugins/__DwoLensInspector', 'Plugins/__DwoStateMonitor', 'Plugins/__DwoCanvas', '__dwo-lens', '__dwo-lens.bat', 'Plugins/__StateMonitor')
        Check 'dwo-forge files hidden from git' (-not $leaks) "visible to git: $leaks"
    }
    Check 'Lens inspector installed' (Test-Path (Join-Path $PluginDest 'Binaries\Win64\UnrealEditor-DwoLensInspector.dll')) 'run attach again'
    Check 'project launcher' (Test-Path $LauncherPath) 'run attach again'
    $py = Get-Command python -ErrorAction SilentlyContinue
    Check 'python available' ($py -ne $null) 'install Python 3.10+'
    if ($uproject.IsCode) {
        Write-Host '  [note] code project: Plugins/__DwoLensInspector is prebuilt, editor-only and disabled by default.'
    }
    $tools = Get-ToolSummary
    Write-Host "  [info] DwoStateMonitor: $($tools.StateMonitor)"
    Check 'no "StateMonitor" module references left' (-not (Write-StaleDepsWarning)) 'rename them to DwoStateMonitor (see above)'
    Write-Host "  [info] DwoCanvas: $($tools.Canvas)"
    if ((Get-CanvasState) -ne 'none') {
        Check 'DwoCanvas validator available' (Test-Path (Join-Path $SystemDir 'canvas\python\dwocanvas')) 'canvas\python missing from the dwo-Forge install'
    }
    return (-not $script:doctorFailed)
}

# ---------------------------------------------------------------- config / engine runs
function Get-ConfigFiles {
    $files = @(Join-Path $SystemDir 'config\default.json')
    $override = Join-Path $ForgeDir 'config.json'
    if (Test-Path $override) { $files += $override }
    return $files
}

function Get-Config {
    # Two-level merge: project config.json overrides keys inside each section of default.json.
    $cfg = Get-Content -Raw -Encoding UTF8 (Join-Path $SystemDir 'config\default.json') | ConvertFrom-Json
    $override = Join-Path $ForgeDir 'config.json'
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
    # The state monitor plugin in any form (installed by dwo-Forge, dwo-Lens 1.x, or the project's own copy).
    $names = @($Config.stateMonitor.pluginName) | Where-Object { $_ } | ForEach-Object { "$_.uplugin" }
    if (-not $names) { $names = @('DwoStateMonitor.uplugin', 'StateMonitor.uplugin') }
    $plugins = Join-Path $Project 'Plugins'
    if (-not (Test-Path $plugins)) { return $null }
    return Get-ChildItem -Path $plugins -Recurse -Include $names -ErrorAction SilentlyContinue |
           Where-Object { $_.FullName -notlike '*\__DwoLensInspector\*' } | Select-Object -First 1
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
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
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
            Say "DwoStateMonitor detected ($($sm.FullName.Substring($Project.Length + 1))): adding '$($cfg.stateMonitor.testFilter)' tests"
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
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
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
    # First non-option argument after the command, e.g. __dwo-forge.bat perf /Game/Maps/L_Main
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
    # process tree. ExitCode is $null when dwo-Forge stopped the process, the real code when it exited on its own.
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
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
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
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
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
    if (-not $state) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
    $run = New-RunDir 'sizes'
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $pyArgs = @((Join-Path $SystemDir 'tools\lens_runtime.py'), 'sizes', '--run', $run.Dir, '--project', $Project, '--config') + @(Get-ConfigFiles)
    $graph = Get-PreviousOutput 'run' 'graph.json' ''
    if ($graph) { $pyArgs += @('--graph', $graph) } else { Say 'No analyze run yet: hard-reference closure weights skipped (run __dwo-forge.bat analyze).' }
    Say "[$($run.Id)] Z1 content sizes"
    & python @pyArgs
    $result = Read-LineResult (Join-Path $run.Dir 'out\sizes.json')
    $stations = @([ordered]@{ id = 'Z1'; name = 'content-sizes'; ms = $sw.ElapsedMilliseconds; verdict = $result.verdict; graph = $graph })
    # No engine run: the version recorded at attach is enough for run.json.
    Complete-Line $run 'sizes' $stations $result ([pscustomobject]@{ Version = $state.engineVersion })
}

function Invoke-Trace {
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
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
    if (-not (Read-State)) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
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
    if (-not $state) { Fail 'Not attached. Run __dwo-forge.bat attach first.' }
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
        $answer = Read-Host '[dwo-forge] Keep the run history and logs? [K]eep (moved out of the project) / [D]elete'
        if ($answer -match '^[kK]') { return 'keep' }
        if ($answer -match '^[dD]') { return 'delete' }
    }
}

function Remove-LegacyLeftovers {
    # dwo-Lens 1.x names that a migration in progress may have left (an empty __dwo-lens, its launcher).
    if (Test-Path $LegacyDir) {
        Get-ChildItem $LegacyDir -Recurse -Force -File | ForEach-Object { if ($_.IsReadOnly) { $_.IsReadOnly = $false } }
        Remove-Item -Recurse -Force $LegacyDir
    }
    # The old launcher deletes itself when it is the one running this uninstall.
    if (-not $FromLegacyBat -and (Test-OurLauncher $LegacyLauncher)) { Remove-Item -Force $LegacyLauncher }
}

function Invoke-Uninstall {
    $state = Read-State
    $canvas = Get-CanvasState
    $anything = $state -or (Test-Path $ForgeDir) -or (Test-Path $PluginDest) -or (Get-InstalledStateMonitor) -or ($canvas -eq 'managed') -or (Test-Path $LegacyDir) -or (Test-OurLauncher $LegacyLauncher)
    if (-not $anything) {
        Say 'dwo-Forge is not attached to this project.'
        Clear-ExcludeBlock | Out-Null
        if ($env:DWO_FORGE_FROM_BAT -ne '1' -and (Test-Path $LauncherPath)) { Remove-Item -Force $LauncherPath }
        return
    }
    $inv = Get-UninstallInventory
    Say 'uninstall: removes everything dwo-Forge added to this project'
    $canvasDirs = if ($canvas -eq 'managed') { @($CanvasDest, $CanvasOldDest) } else { @() }
    Write-Host "    plugins     : $(@(@($PluginDest, $SmDest, $SmLegacyDest) + $canvasDirs | Where-Object { Test-Path $_ } | ForEach-Object { Split-Path -Leaf $_ }) -join ', ')"
    Write-Host "    launcher    : __dwo-forge.bat$(if (Test-OurLauncher $LegacyLauncher) { ', __dwo-lens.bat (dwo-Lens 1.x)' })"
    Write-Host "    exclude     : $(if ((Test-BlockPresent) -or (Test-BlockPresent -Legacy)) { (Get-Vcs).Exclude } else { 'none' })"
    Write-Host ("    run history : {0} runs, {1:N1} MB (__dwo-forge\data)" -f $inv.Runs.Count, ($inv.DataBytes / 1MB))
    Write-Host "    other files : $($inv.Created.Count) (perf CSV captures in Saved\Profiling)"

    # Check DwoCanvas users before removing anything, so a refusal leaves the attach intact.
    if ($canvas -eq 'managed' -and -not ($Rest -contains '--force') -and @(Get-CanvasUsers).Count -gt 0) {
        $null = Remove-Canvas $false   # prints the assets that use it
        Fail 'Uninstall stopped. DwoCanvas is still in use.'
    }
    $choice = Read-DataChoice $inv
    if (-not (Remove-StateMonitor ($Rest -contains '--force'))) { Fail 'Uninstall stopped. DwoStateMonitor is still in use.' }
    if ($canvas -eq 'managed' -and -not (Remove-Canvas ($Rest -contains '--force'))) { Fail 'Uninstall stopped. DwoCanvas is still in use.' }
    Remove-Plugin

    if ($choice -eq 'keep') {
        $dest = Join-Path $AppDataDir ("uninstalled\" + (Split-Path -Leaf $Project) + '-' + (Get-Date).ToString('yyyyMMdd-HHmmss'))
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
    # Engine folders dwo-Forge runs created and that are now empty (for example Saved\Profiling\CSV).
    foreach ($f in $inv.Created) {
        $dir = Split-Path -Parent (Join-Path $Project $f)
        while ($dir -and $dir.Length -gt $Project.Length -and (Test-Path $dir) -and -not (Get-ChildItem -Force $dir)) {
            Remove-Item -Force $dir
            $dir = Split-Path -Parent $dir
        }
    }

    $cleared = Clear-ExcludeBlock
    Set-Location $Project
    if (Test-Path $ForgeDir) {
        Get-ChildItem $ForgeDir -Recurse -Force -File | ForEach-Object { if ($_.IsReadOnly) { $_.IsReadOnly = $false } }
        Remove-Item -Recurse -Force $ForgeDir
    }
    Remove-LegacyLeftovers
    # A launcher run by itself deletes its own file after this script returns.
    if ($env:DWO_FORGE_FROM_BAT -ne '1' -and (Test-Path $LauncherPath)) { Remove-Item -Force $LauncherPath }

    $left = @(@($PluginDest, $SmDest, $SmLegacyDest, $CanvasDest, $ForgeDir, $LegacyDir) | Where-Object { Test-Path $_ })
    if (Test-Path (Join-Path $CanvasOldDest $CanvasOldMarker)) { $left += $CanvasOldDest }
    if ($env:DWO_FORGE_FROM_BAT -ne '1' -and (Test-Path $LauncherPath)) { $left += $LauncherPath }
    if ($left.Count -gt 0) { Say "Uninstalled, but these remain: $($left -join ', ')"; exit 3 }
    if ($cleared) { Say 'Exclude entries removed.' }
    $vcs = Get-Vcs
    if ($vcs.Kind -ne 'none' -and $state -and $state.gitStatusBeforeAttach -ne $null) {
        # Ignored: the running launcher (it deletes itself next), and a 2.0.0-era visible Plugins\DwoCanvas.
        $after = (@((Get-ProjectStatus) -split "`n") | Where-Object { $_ -and $_ -notmatch '__dwo-(forge|lens)\.bat"?$' -and $_ -notmatch 'Plugins/DwoCanvas/' }) -join "`n"
        $before = (@("$($state.gitStatusBeforeAttach)" -split "`n") | Where-Object { $_ -and $_ -notmatch 'Plugins/DwoCanvas/' }) -join "`n"
        if ($after -ne $before) { Say 'Uninstalled, but git status differs from before attach. Review: git status'; exit 3 }
        Say 'Uninstalled. git status matches the state before attach.'
    } else {
        Say 'Uninstalled. No dwo-Forge files remain in the project.'
    }
}

# Every command first migrates what dwo-Lens 1.x left in the project ('tools' does not touch a project).
if ($Command -ne 'tools') { Invoke-Migration }

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
    'canvas-install'  { Invoke-CanvasInstall }
    'canvas-remove'   { Invoke-CanvasRemove }
    'canvas-import'   { Invoke-CanvasImport }
    'canvas-validate' { Invoke-CanvasValidate }
    'gui'     { & (Join-Path $SystemDir 'tools\gui.ps1') -SystemDir $SystemDir -Project $Project -ForgeHome $SystemDir }
    'uninstall' { Invoke-Uninstall }
    'detach'  { Invoke-Uninstall }
}

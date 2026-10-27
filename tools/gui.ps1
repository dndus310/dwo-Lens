# Builds (once per source version) and launches the dwo-Assay ImGui control panel.
# Windows PowerShell 5.1 compatible.
param(
    [Parameter(Mandatory = $true)][string]$SystemDir,
    [string]$Project = '',
    [string]$AssayHome = '',
    [switch]$BuildOnly
)

$ErrorActionPreference = 'Stop'
function Say([string]$Message) { Write-Host "[dwo-assay] $Message" }
function Fail([string]$Message) { Write-Host "[dwo-assay] ERROR: $Message" -ForegroundColor Red; exit 1 }

$guiDir = Join-Path $SystemDir 'gui'

function Get-SourceHash {
    $sha = [Security.Cryptography.SHA256]::Create()
    $sb = New-Object System.Text.StringBuilder
    Get-ChildItem -Recurse -File $guiDir | Sort-Object FullName | ForEach-Object {
        [void]$sb.Append($_.FullName.Substring($guiDir.Length) + '=' + (Get-FileHash -Algorithm SHA256 $_.FullName).Hash + ';')
    }
    $bytes = $sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($sb.ToString()))
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 12)
}

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { Fail 'Visual Studio (vswhere.exe) not found. Install Visual Studio with the C++ workload.' }
    $install = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $install) { Fail 'No Visual Studio installation with C++ x64 tools found.' }
    $vcvars = Join-Path $install 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { Fail "vcvars64.bat not found under $install" }
    return $vcvars
}

$hash = Get-SourceHash
$cache = Join-Path $env:LOCALAPPDATA "dwo-Assay\gui-cache\$hash"
$exe = Join-Path $cache 'dwo-assay-gui.exe'

if (-not (Test-Path $exe)) {
    Say "Building the GUI (first time for this version, about a minute)..."
    $vcvars = Find-VcVars
    $obj = Join-Path $cache 'obj'
    New-Item -ItemType Directory -Force $obj | Out-Null
    $tp = Join-Path $guiDir 'third_party'
    $im = Join-Path $tp 'imgui'
    $sources = @(
        (Join-Path $guiDir 'src\main.cpp'),
        (Join-Path $im 'imgui.cpp'), (Join-Path $im 'imgui_draw.cpp'), (Join-Path $im 'imgui_tables.cpp'),
        (Join-Path $im 'imgui_widgets.cpp'), (Join-Path $im 'imgui_demo.cpp'),
        (Join-Path $im 'backends\imgui_impl_win32.cpp'), (Join-Path $im 'backends\imgui_impl_dx11.cpp')
    ) | ForEach-Object { "`"$_`"" }
    $log = Join-Path $cache 'build.log'
    $cl = "cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /DUNICODE /D_UNICODE /DNOMINMAX " +
          "/I`"$im`" /I`"$tp`" /Fo`"$obj\\`" /Fe`"$exe`" " + ($sources -join ' ') +
          " /link /SUBSYSTEM:WINDOWS d3d11.lib d3dcompiler.lib dxgi.lib user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib dwmapi.lib"
    & cmd /c "call `"$vcvars`" >nul && $cl > `"$log`" 2>&1"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        Get-Content $log -Tail 30 | ForEach-Object { Write-Host $_ }
        Fail "GUI build failed. See $log"
    }
    Remove-Item -Recurse -Force $obj
    Say "Built $exe"
}

if ($BuildOnly) { exit 0 }

$argList = @()
if ($Project) { $argList += @('--project', "`"$Project`"") }
if ($AssayHome) { $argList += @('--home', "`"$AssayHome`"") }
if ($argList.Count -gt 0) { Start-Process -FilePath $exe -ArgumentList ($argList -join ' ') }
else { Start-Process -FilePath $exe }
Say 'GUI started.'

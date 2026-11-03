@echo off
rem ============================================================================
rem  dwo-Lens bootstrap. Copy this file to the root of an Unreal project and run:
rem    __dwo-lens.bat [attach|update|status|analyze|verify|logs|smoke|perf|sizes|trace|play|...] [map|--keep-data]
rem  This file only bootstraps. Everything else lives in __dwo-lens\system (a git
rem  clone of the dwo-Lens repository) so it always matches the installed version.
rem ============================================================================
setlocal EnableExtensions
set "PROJ=%~dp0"
set "PROJ=%PROJ:~0,-1%"
set "CMD=%~1"
if "%CMD%"=="" set "CMD=attach"
set "EXTRA=%~2"

rem ---- 1. preflight ----
dir /b "%PROJ%\*.uproject" >nul 2>&1 || (echo [dwo-lens] No .uproject next to this file. & exit /b 1)
where git >nul 2>&1 || (echo [dwo-lens] git is not installed or not on PATH. & exit /b 1)
for /f "delims=" %%i in ('git -C "%PROJ%" rev-parse --path-format^=absolute --git-path info/exclude 2^>nul') do set "EXCL=%%i"
if not defined EXCL (echo [dwo-lens] This folder is not inside a git repository. & exit /b 1)

set "SYS=%PROJ%\__dwo-lens\system"

if /i "%CMD%"=="attach" goto :fetch
if /i "%CMD%"=="update" goto :fetch
if /i "%CMD%"=="status" goto :handoff
if /i "%CMD%"=="analyze" goto :handoff
if /i "%CMD%"=="verify" goto :handoff
if /i "%CMD%"=="logs" goto :handoff
if /i "%CMD%"=="smoke" goto :handoff
if /i "%CMD%"=="perf" goto :handoff
if /i "%CMD%"=="sizes" goto :handoff
if /i "%CMD%"=="trace" goto :handoff
if /i "%CMD%"=="play" goto :handoff
if /i "%CMD%"=="tools" goto :handoff
if /i "%CMD%"=="gui" goto :handoff
if /i "%CMD%"=="sm-install" goto :handoff
if /i "%CMD%"=="sm-remove" goto :handoff
if /i "%CMD%"=="doctor" goto :handoff
if /i "%CMD%"=="detach" goto :handoff
echo Usage: __dwo-lens.bat [attach^|update^|status^|analyze^|verify^|logs^|smoke^|perf^|sizes^|trace^|play^|tools^|gui^|sm-install^|sm-remove^|doctor^|detach] [map^|--keep-data^|--force^|--runtime]
exit /b 1

:fetch
if not defined DWO_LENS_HOME (
  echo [dwo-lens] DWO_LENS_HOME is not set. Run setup.bat in the dwo-Lens repository once.
  exit /b 1
)
if not exist "%DWO_LENS_HOME%\.git" (echo [dwo-lens] DWO_LENS_HOME does not point to the dwo-Lens repository: %DWO_LENS_HOME% & exit /b 1)

rem ---- 2. exclude block before any file is created ----
for %%d in ("%EXCL%") do if not exist "%%~dpd" mkdir "%%~dpd"
findstr /c:"# >>> dwo-lens managed" "%EXCL%" >nul 2>&1 || (
  >>"%EXCL%" echo.
  >>"%EXCL%" echo # ^>^>^> dwo-lens managed ^(do not edit^) ^>^>^>
  >>"%EXCL%" echo /__dwo-lens/
  >>"%EXCL%" echo /__dwo-lens.bat
  >>"%EXCL%" echo /Plugins/__DwoLensInspector/
  >>"%EXCL%" echo # ^<^<^< dwo-lens managed ^<^<^<
)

rem ---- 3. clone or pull the system ----
if exist "%SYS%\.git" (
  git -C "%SYS%" pull --ff-only || (echo [dwo-lens] git pull failed. & exit /b 1)
) else (
  git clone --quiet "%DWO_LENS_HOME%" "%SYS%" || (echo [dwo-lens] git clone failed. & exit /b 1)
)

:handoff
if not exist "%SYS%\tools\lens.ps1" (echo [dwo-lens] Not attached. Run: __dwo-lens.bat attach & exit /b 1)
cd /d "%PROJ%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SYS%\tools\lens.ps1" -Command %CMD% -Project "%PROJ%" %EXTRA%
set "RC=%ERRORLEVEL%"

rem 0 = clean, 3 = detached but git status differs; either way the system is gone.
set "GONE="
if "%RC%"=="0" set "GONE=1"
if "%RC%"=="3" set "GONE=1"
if /i "%CMD%"=="detach" if defined GONE (
  echo [dwo-lens] Removing __dwo-lens.bat
  (goto) 2>nul & del "%~f0"
)
exit /b %RC%

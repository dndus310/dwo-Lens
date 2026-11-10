@echo off
rem ============================================================================
rem  dwo-Forge project launcher. Placed in the project root by attach and removed
rem  by uninstall. It runs dwo-Forge from its install folder (no copy of the system
rem  lives in the project).
rem    __dwo-forge.bat                     open the control panel (attach first if needed)
rem    Lens            analyze^|verify^|logs^|smoke^|perf^|sizes^|trace^|play
rem    DwoStateMonitor sm-install [--runtime]^|sm-remove [--force]
rem    DwoCanvas       canvas-install^|canvas-remove [--force]^|canvas-validate ^<file^>
rem                    canvas-import ^<file^> [--reset] [--doc Name]
rem    status^|doctor^|update^|tools^|uninstall [--keep-data^|--delete-data] [--force]
rem ============================================================================
setlocal EnableExtensions
set "PROJ=%~dp0"
set "PROJ=%PROJ:~0,-1%"
set "CMD=%~1"
if "%CMD%"=="" (
  if exist "%PROJ%\__dwo-forge\state.json" (set "CMD=gui") else (set "CMD=attach")
)

rem ---- find the dwo-Forge install folder: recorded at attach, else DWO_FORGE_HOME (DWO_LENS_HOME before 2.0) ----
set "HOME_DIR="
if exist "%PROJ%\__dwo-forge\home.txt" set /p HOME_DIR=<"%PROJ%\__dwo-forge\home.txt"
if not exist "%HOME_DIR%\tools\forge.ps1" set "HOME_DIR=%DWO_FORGE_HOME%"
if not exist "%HOME_DIR%\tools\forge.ps1" set "HOME_DIR=%DWO_LENS_HOME%"
if not exist "%HOME_DIR%\tools\forge.ps1" (
  echo [dwo-forge] dwo-Forge was not found. Run dwo-Forge.bat in the dwo-Forge folder once, then try again.
  exit /b 1
)
dir /b "%PROJ%\*.uproject" >nul 2>&1 || (echo [dwo-forge] No .uproject next to this file. & exit /b 1)

set "DWO_FORGE_FROM_BAT=1"
set "DWO_LENS_FROM_BAT="
cd /d "%PROJ%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\forge.ps1" -Command %CMD% -Project "%PROJ%" %2 %3 %4 %5 %6 %7 %8 %9
set "RC=%ERRORLEVEL%"

rem 0 = clean, 3 = removed but something differs; either way dwo-Forge is gone, so is this file.
set "GONE="
if "%RC%"=="0" set "GONE=1"
if "%RC%"=="3" set "GONE=1"
if /i "%CMD%"=="detach" set "CMD=uninstall"
if /i "%CMD%"=="uninstall" if defined GONE (
  echo [dwo-forge] Removing __dwo-forge.bat
  (goto) 2>nul & del "%~f0"
)
rem update replaces this file after it finished running
if exist "%PROJ%\__dwo-forge\launcher.next" (
  (goto) 2>nul & move /y "%PROJ%\__dwo-forge\launcher.next" "%~f0" >nul
)
exit /b %RC%

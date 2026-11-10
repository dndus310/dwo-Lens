@echo off
rem ============================================================================
rem  dwo-Lens project launcher. Placed in the project root by attach and removed
rem  by uninstall. It runs dwo-Lens from its install folder (no copy of the system
rem  lives in the project).
rem    __dwo-lens.bat                     open the control panel (attach first if needed)
rem    __dwo-lens.bat analyze^|verify^|logs^|smoke^|perf^|sizes^|trace^|play
rem    __dwo-lens.bat status^|doctor^|update^|sm-install^|sm-remove^|tools
rem    __dwo-lens.bat uninstall [--keep-data^|--delete-data] [--force]
rem ============================================================================
setlocal EnableExtensions
set "PROJ=%~dp0"
set "PROJ=%PROJ:~0,-1%"
set "CMD=%~1"
if "%CMD%"=="" (
  if exist "%PROJ%\__dwo-lens\state.json" (set "CMD=gui") else (set "CMD=attach")
)

rem ---- find the dwo-Lens install folder: recorded at attach, else DWO_LENS_HOME ----
set "HOME_DIR="
if exist "%PROJ%\__dwo-lens\home.txt" set /p HOME_DIR=<"%PROJ%\__dwo-lens\home.txt"
if not exist "%HOME_DIR%\tools\lens.ps1" set "HOME_DIR=%DWO_LENS_HOME%"
if not exist "%HOME_DIR%\tools\lens.ps1" (
  echo [dwo-lens] dwo-Lens was not found. Run dwo-Lens.bat in the dwo-Lens folder once, then try again.
  exit /b 1
)
dir /b "%PROJ%\*.uproject" >nul 2>&1 || (echo [dwo-lens] No .uproject next to this file. & exit /b 1)

set "DWO_LENS_FROM_BAT=1"
cd /d "%PROJ%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\lens.ps1" -Command %CMD% -Project "%PROJ%" %2 %3 %4 %5
set "RC=%ERRORLEVEL%"

rem 0 = clean, 3 = removed but something differs; either way dwo-Lens is gone, so is this file.
set "GONE="
if "%RC%"=="0" set "GONE=1"
if "%RC%"=="3" set "GONE=1"
if /i "%CMD%"=="detach" set "CMD=uninstall"
if /i "%CMD%"=="uninstall" if defined GONE (
  echo [dwo-lens] Removing __dwo-lens.bat
  (goto) 2>nul & del "%~f0"
)
rem update replaces this file after it finished running
if exist "%PROJ%\__dwo-lens\launcher.next" (
  (goto) 2>nul & move /y "%PROJ%\__dwo-lens\launcher.next" "%~f0" >nul
)
exit /b %RC%

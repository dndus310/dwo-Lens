@echo off
rem ============================================================================
rem  dwo-Lens. Double-click to open the control panel. No setup step is needed:
rem  the first run registers this folder and builds the window (about a minute).
rem    dwo-Lens.bat                                open the control panel
rem    dwo-Lens.bat <project folder or .uproject>  open it on that project (drag and drop works)
rem    dwo-Lens.bat <command> <project folder> [options]
rem        attach, uninstall [--keep-data^|--delete-data], analyze, verify, logs, smoke, perf,
rem        sizes, trace, play, status, doctor, update, sm-install, sm-remove, tools
rem ============================================================================
setlocal EnableExtensions
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"

rem ---- register this folder for __dwo-lens.bat launchers in projects ----
if /i not "%DWO_LENS_HOME%"=="%HOME_DIR%" (
  setx DWO_LENS_HOME "%HOME_DIR%" >nul 2>&1
  set "DWO_LENS_HOME=%HOME_DIR%"
)

set "ARG=%~1"
if "%ARG%"=="" goto :gui
if /i "%~x1"==".uproject" (
  set "ARG=%~dp1"
  goto :project
)
pushd "%ARG%" 2>nul && (popd & goto :project)

rem ---- command line: dwo-Lens.bat <command> <project> ----
set "PROJ=%~2"
if "%PROJ%"=="" set "PROJ=%CD%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\lens.ps1" -Command %1 -Project "%PROJ%" %3 %4 %5
exit /b %ERRORLEVEL%

:project
if "%ARG:~-1%"=="\" set "ARG=%ARG:~0,-1%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\gui.ps1" -SystemDir "%HOME_DIR%" -LensHome "%HOME_DIR%" -Project "%ARG%"
if errorlevel 1 pause
exit /b

:gui
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\gui.ps1" -SystemDir "%HOME_DIR%" -LensHome "%HOME_DIR%"
if errorlevel 1 pause
exit /b

@echo off
rem ============================================================================
rem  dwo-Forge. Double-click to open the control panel. No setup step is needed:
rem  the first run registers this folder and builds the window (about a minute).
rem    dwo-Forge.bat                                open the control panel
rem    dwo-Forge.bat <project folder or .uproject>  open it on that project (drag and drop works)
rem    dwo-Forge.bat <command> <project folder> [options]
rem        attach, update, status, doctor, tools, uninstall [--keep-data^|--delete-data]
rem        Lens            analyze, verify, logs, smoke, perf, sizes, trace, play
rem        DwoStateMonitor sm-install [--runtime], sm-remove
rem        DwoCanvas       canvas-install, canvas-remove, canvas-validate ^<file^>,
rem                        canvas-import ^<file^> [--reset] [--doc Name]
rem ============================================================================
setlocal EnableExtensions
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"

rem ---- register this folder for __dwo-forge.bat launchers in projects ----
if /i not "%DWO_FORGE_HOME%"=="%HOME_DIR%" (
  setx DWO_FORGE_HOME "%HOME_DIR%" >nul 2>&1
  set "DWO_FORGE_HOME=%HOME_DIR%"
)

set "ARG=%~1"
if "%ARG%"=="" goto :gui
if /i "%~x1"==".uproject" (
  set "ARG=%~dp1"
  goto :project
)
pushd "%ARG%" 2>nul && (popd & goto :project)

rem ---- command line: dwo-Forge.bat <command> <project> ----
set "PROJ=%~2"
if "%PROJ%"=="" set "PROJ=%CD%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\forge.ps1" -Command %1 -Project "%PROJ%" %3 %4 %5 %6 %7 %8 %9
exit /b %ERRORLEVEL%

:project
if "%ARG:~-1%"=="\" set "ARG=%ARG:~0,-1%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\gui.ps1" -SystemDir "%HOME_DIR%" -ForgeHome "%HOME_DIR%" -Project "%ARG%"
if errorlevel 1 pause
exit /b

:gui
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\gui.ps1" -SystemDir "%HOME_DIR%" -ForgeHome "%HOME_DIR%"
if errorlevel 1 pause
exit /b

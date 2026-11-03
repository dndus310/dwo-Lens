@echo off
rem Registers this dwo-Lens repository as DWO_LENS_HOME for the current Windows user.
rem Run once per machine. Open a new terminal afterwards.
setlocal
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"
if not exist "%HOME_DIR%\.git" (echo [dwo-lens] This folder is not a git repository: %HOME_DIR% & exit /b 1)
setx DWO_LENS_HOME "%HOME_DIR%" >nul || exit /b 1
echo [dwo-lens] DWO_LENS_HOME=%HOME_DIR%
echo [dwo-lens] Open a new terminal, copy __dwo-lens.bat into an Unreal project root, and run it.

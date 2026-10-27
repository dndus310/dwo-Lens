@echo off
rem Registers this dwo-Assay repository as DWO_ASSAY_HOME for the current Windows user.
rem Run once per machine. Open a new terminal afterwards.
setlocal
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"
if not exist "%HOME_DIR%\.git" (echo [dwo-assay] This folder is not a git repository: %HOME_DIR% & exit /b 1)
setx DWO_ASSAY_HOME "%HOME_DIR%" >nul || exit /b 1
echo [dwo-assay] DWO_ASSAY_HOME=%HOME_DIR%
echo [dwo-assay] Open a new terminal, copy __dwo-assay.bat into an Unreal project root, and run it.

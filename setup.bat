@echo off
rem Optional. dwo-Forge.bat registers the folder by itself on first run.
rem Use this only to register DWO_FORGE_HOME without opening the window.
setlocal
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"
setx DWO_FORGE_HOME "%HOME_DIR%" >nul || exit /b 1
echo [dwo-forge] DWO_FORGE_HOME=%HOME_DIR%
echo [dwo-forge] Next: double-click dwo-Forge.bat, open your project and press Attach.

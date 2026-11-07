@echo off
rem Optional. dwo-Lens.bat registers the folder by itself on first run.
rem Use this only to register DWO_LENS_HOME without opening the window.
setlocal
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"
setx DWO_LENS_HOME "%HOME_DIR%" >nul || exit /b 1
echo [dwo-lens] DWO_LENS_HOME=%HOME_DIR%
echo [dwo-lens] Next: double-click dwo-Lens.bat, open your project and press Attach.

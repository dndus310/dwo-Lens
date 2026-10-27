@echo off
rem Opens the dwo-Assay control panel. Double-click this file in the dwo-Assay repository.
rem The first run builds the GUI with Visual Studio C++ tools (about a minute).
setlocal
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tools\gui.ps1" -SystemDir "%HOME_DIR%" -AssayHome "%HOME_DIR%"
if errorlevel 1 pause

@echo off
rem Registers this Archi repository as ARCHI_HOME for the current Windows user.
rem Run once per machine. Open a new terminal afterwards.
setlocal
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"
if not exist "%HOME_DIR%\.git" (echo [archi] This folder is not a git repository: %HOME_DIR% & exit /b 1)
setx ARCHI_HOME "%HOME_DIR%" >nul || exit /b 1
echo [archi] ARCHI_HOME=%HOME_DIR%
echo [archi] Open a new terminal, copy __archi.bat into an Unreal project root, and run it.

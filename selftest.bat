@echo off
rem dwo-Lens self verification: unit tests, static checks, GUI build and a full command cycle
rem on an isolated copy of the given Unreal project (the project itself is never modified).
rem   selftest.bat "C:\path\to\SomeProject"
setlocal
set "HOME_DIR=%~dp0"
set "HOME_DIR=%HOME_DIR:~0,-1%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tests\selftest.ps1" -Project "%~1"
set "RC=%ERRORLEVEL%"
echo.
echo Result: %HOME_DIR%\.qa\selftest\selftest-result.json
exit /b %RC%

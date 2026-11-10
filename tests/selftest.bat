@echo off
rem dwo-Forge self verification: unit tests, static checks, GUI build and a full command cycle
rem on an isolated copy of the given Unreal project (the project itself is never modified).
rem   tests\selftest.bat "C:\path\to\SomeProject"
setlocal
for %%I in ("%~dp0..") do set "HOME_DIR=%%~fI"
powershell -NoProfile -ExecutionPolicy Bypass -File "%HOME_DIR%\tests\selftest.ps1" -Project "%~1"
set "RC=%ERRORLEVEL%"
echo.
echo Result: %HOME_DIR%\.qa\selftest\selftest-result.json
exit /b %RC%

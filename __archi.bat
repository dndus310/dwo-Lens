@echo off
rem ============================================================================
rem  Archi bootstrap. Copy this file to the root of an Unreal project and run:
rem    __archi.bat [attach|update|status|analyze|doctor|detach] [--keep-data]
rem  This file only bootstraps. Everything else lives in __archi\system (a git
rem  clone of the Archi repository) so it always matches the installed version.
rem ============================================================================
setlocal EnableExtensions
set "PROJ=%~dp0"
set "PROJ=%PROJ:~0,-1%"
set "CMD=%~1"
if "%CMD%"=="" set "CMD=attach"
set "EXTRA=%~2"

rem ---- 1. preflight ----
dir /b "%PROJ%\*.uproject" >nul 2>&1 || (echo [archi] No .uproject next to this file. & exit /b 1)
where git >nul 2>&1 || (echo [archi] git is not installed or not on PATH. & exit /b 1)
for /f "delims=" %%i in ('git -C "%PROJ%" rev-parse --path-format^=absolute --git-path info/exclude 2^>nul') do set "EXCL=%%i"
if not defined EXCL (echo [archi] This folder is not inside a git repository. & exit /b 1)

set "SYS=%PROJ%\__archi\system"

if /i "%CMD%"=="attach" goto :fetch
if /i "%CMD%"=="update" goto :fetch
if /i "%CMD%"=="status" goto :handoff
if /i "%CMD%"=="analyze" goto :handoff
if /i "%CMD%"=="doctor" goto :handoff
if /i "%CMD%"=="detach" goto :handoff
echo Usage: __archi.bat [attach^|update^|status^|analyze^|doctor^|detach] [--keep-data]
exit /b 1

:fetch
if not defined ARCHI_HOME (
  echo [archi] ARCHI_HOME is not set. Run setup.bat in the Archi repository once.
  exit /b 1
)
if not exist "%ARCHI_HOME%\.git" (echo [archi] ARCHI_HOME does not point to the Archi repository: %ARCHI_HOME% & exit /b 1)

rem ---- 2. exclude block before any file is created ----
for %%d in ("%EXCL%") do if not exist "%%~dpd" mkdir "%%~dpd"
findstr /c:"# >>> archi managed" "%EXCL%" >nul 2>&1 || (
  >>"%EXCL%" echo.
  >>"%EXCL%" echo # ^>^>^> archi managed ^(do not edit^) ^>^>^>
  >>"%EXCL%" echo /__archi/
  >>"%EXCL%" echo /__archi.bat
  >>"%EXCL%" echo /Plugins/__ArchiInspector/
  >>"%EXCL%" echo # ^<^<^< archi managed ^<^<^<
)

rem ---- 3. clone or pull the system ----
if exist "%SYS%\.git" (
  git -C "%SYS%" pull --ff-only || (echo [archi] git pull failed. & exit /b 1)
) else (
  git clone --quiet "%ARCHI_HOME%" "%SYS%" || (echo [archi] git clone failed. & exit /b 1)
)

:handoff
if not exist "%SYS%\tools\archi.ps1" (echo [archi] Not attached. Run: __archi.bat attach & exit /b 1)
cd /d "%PROJ%"
powershell -NoProfile -ExecutionPolicy Bypass -File "%SYS%\tools\archi.ps1" -Command %CMD% -Project "%PROJ%" %EXTRA%
set "RC=%ERRORLEVEL%"

rem 0 = clean, 3 = detached but git status differs; either way the system is gone.
set "GONE="
if "%RC%"=="0" set "GONE=1"
if "%RC%"=="3" set "GONE=1"
if /i "%CMD%"=="detach" if defined GONE (
  echo [archi] Removing __archi.bat
  (goto) 2>nul & del "%~f0"
)
exit /b %RC%

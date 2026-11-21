@echo off
rem dwo-Forge legacy launcher. dwo-Lens is now dwo-Forge and this project's launcher is __dwo-forge.bat.
rem This file only forwards to it once, then deletes itself.
setlocal EnableExtensions
set "PROJ=%~dp0"
echo [dwo-forge] __dwo-lens.bat is deprecated: use __dwo-forge.bat (this file now removes itself).
if not exist "%PROJ%__dwo-forge.bat" (
  echo [dwo-forge] __dwo-forge.bat was not found. Run dwo-Forge.bat in the dwo-Forge folder and attach again.
  (goto) 2>nul & del "%~f0"
)
(goto) 2>nul & del "%~f0" & call "%PROJ%__dwo-forge.bat" %*

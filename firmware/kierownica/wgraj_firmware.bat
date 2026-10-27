@echo off
rem Wgrywa firmware kierownicy do Arduino Due - logika w wgraj_firmware.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0wgraj_firmware.ps1" %*
echo.
pause

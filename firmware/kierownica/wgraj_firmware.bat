@echo off
rem Wgrywa firmware kierownicy (Due, Mega, Uno, Nano, Leonardo, Micro, ESP32, ESP32-S3)
rem - logika i opis parametrow w wgraj_firmware.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0wgraj_firmware.ps1" %*
echo.
pause

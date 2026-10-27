@echo off
rem Uruchamia panel kierownicy. Szuka Pythona przez launcher "py",
rem a jesli go nie ma - przez "python" z PATH.
setlocal
cd /d "%~dp0"

where py >nul 2>nul
if %errorlevel%==0 (
    py -3 kierownica.py %*
    goto :end
)

where python >nul 2>nul
if %errorlevel%==0 (
    python kierownica.py %*
    goto :end
)

echo Nie znaleziono Pythona. Zainstaluj go z https://www.python.org/downloads/
echo i zaznacz "Add Python to PATH".
pause

:end
if errorlevel 1 pause
endlocal

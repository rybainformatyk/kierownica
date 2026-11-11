@echo off
rem Wydanie panelu: build, testy, wersja przenosna (ZIP) i instalator.
rem Wynik w dist\ (stara zawartosc jest usuwana):
rem   Kierownica-X.Y-instalator.exe   instalator ze sterownikami (Inno Setup 6)
rem   Kierownica-X.Y-przenosna.zip    bez instalacji, ustawienia obok programu
rem   Kierownica.exe                  sam program - aktualizacja wersji przenosnej
rem   SHA256SUMS.txt                  sumy kontrolne (bez nich panel nie zaktualizuje sie sam)
rem Wersja: src\version.h. Publikacja na GitHubie: opublikuj.bat
setlocal
cd /d "%~dp0"

call "%~dp0build.bat" || exit /b 1

echo.
echo === Testy ===
"%~dp0bin\tests\core_tests.exe" || (echo Testy nie przeszly - wydanie przerwane. & exit /b 1)
set "SELFTEST=%TEMP%\kierownica_selftest_%RANDOM%"
start /wait "" "%~dp0bin\Kierownica.exe" --selftest "%SELFTEST%"
if errorlevel 1 (
    echo Selftest okna nie przeszedl - szczegoly: %SELFTEST%\selftest.log
    exit /b 1
)
rmdir /s /q "%SELFTEST%" 2>nul
echo Selftest okna: OK

rem --- pakiety: logika w PowerShell (ZIP, wersja z exe, sumy) ---
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference = 'Stop';" ^
  "$v = (Get-Item 'bin\Kierownica.exe').VersionInfo;" ^
  "$ver = '{0}.{1}' -f $v.FileMajorPart, $v.FileMinorPart; if ($v.FileBuildPart) { $ver += '.' + $v.FileBuildPart };" ^
  "if (Test-Path dist) { Remove-Item -Recurse -Force dist };" ^
  "New-Item -ItemType Directory -Force dist | Out-Null;" ^
  "Copy-Item 'bin\Kierownica.exe' 'dist\Kierownica.exe';" ^
  "$stage = Join-Path $env:TEMP ('kierownica_zip_' + [guid]::NewGuid());" ^
  "New-Item -ItemType Directory $stage | Out-Null;" ^
  "Copy-Item 'bin\Kierownica.exe' $stage;" ^
  "Copy-Item '..\..\LICENSE' (Join-Path $stage 'LICENSE.txt');" ^
  "Copy-Item 'installer\CZYTAJ.txt' $stage;" ^
  "Set-Content -Encoding UTF8 (Join-Path $stage 'portable.txt') 'Ten plik sprawia, ze panel zapisuje ustawienia (config.json) obok Kierownica.exe. Bez niego trafiaja do %%APPDATA%%\KierownicaPanel.';" ^
  "$zip = Join-Path 'dist' ('Kierownica-' + $ver + '-przenosna.zip');" ^
  "if (Test-Path $zip) { Remove-Item $zip };" ^
  "Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip;" ^
  "Remove-Item -Recurse -Force $stage;" ^
  "Write-Host ('Wersja przenosna: ' + $zip)" || exit /b 1

rem --- instalator ---
set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not exist "%ISCC%" (
    echo.
    echo Brak Inno Setup 6 - pomijam instalator. Instalacja: winget install JRSoftware.InnoSetup
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\pobierz_sterowniki.ps1" || (echo Nie udalo sie przygotowac sterownikow - wydanie przerwane. & exit /b 1)
    "%ISCC%" /Q installer\kierownica.iss || exit /b 1
)

powershell -NoProfile -Command ^
  "Get-ChildItem dist -File | Where-Object { $_.Extension -in '.exe', '.zip' } | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name } | Set-Content -Encoding ASCII dist\SHA256SUMS.txt; Get-Content dist\SHA256SUMS.txt"

echo.
echo Gotowe: %~dp0dist
endlocal

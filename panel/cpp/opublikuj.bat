@echo off
rem Publikuje zawartosc dist\ jako wydanie na GitHubie (rybainformatyk/kierownica).
rem Najpierw: wydanie.bat. Potrzebny gh (GitHub CLI) zalogowany do repozytorium.
rem
rem   opublikuj.bat                 wydanie vX.Y z opisem z installer\NOWOSCI.md
rem
rem Panele w wersji 3.2+ znajda je same (Diagnostyka - Aktualizacje) i zaktualizuja
rem sie po sprawdzeniu SHA256SUMS.txt.
setlocal
cd /d "%~dp0"
if not exist dist\SHA256SUMS.txt (
    echo Brak dist\SHA256SUMS.txt - najpierw uruchom wydanie.bat
    exit /b 1
)
for /f "usebackq tokens=*" %%v in (`powershell -NoProfile -Command "$v = (Get-Item 'dist\Kierownica.exe').VersionInfo; $t = '{0}.{1}' -f $v.FileMajorPart, $v.FileMinorPart; if ($v.FileBuildPart) { $t += '.' + $v.FileBuildPart }; $t"`) do set "VER=%%v"
if not defined VER (
    echo Nie udalo sie odczytac wersji z dist\Kierownica.exe
    exit /b 1
)
set "NOTES=installer\NOWOSCI.md"
if not exist "%NOTES%" (
    echo Brak %NOTES% - opisz w nim zmiany tej wersji.
    exit /b 1
)
echo Publikuje wydanie v%VER% na GitHubie:
dir /b dist
choice /m "Kontynuowac"
if errorlevel 2 exit /b 1
gh release create "v%VER%" dist\Kierownica-%VER%-instalator.exe dist\Kierownica-%VER%-przenosna.zip dist\Kierownica.exe dist\SHA256SUMS.txt --repo rybainformatyk/kierownica --title "Kierownica %VER%" --notes-file "%NOTES%" || exit /b 1
echo.
echo Opublikowano v%VER%.
endlocal

@echo off
rem Buduje panel w C++ (Visual Studio 2022 / Build Tools, Release x64).
rem Wynik: bin\Kierownica.exe
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Nie znaleziono Visual Studio. Zainstaluj Build Tools 2022 z komponentem C++.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
    echo Visual Studio nie ma narzedzi C++ x64.
    exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
if exist "%NINJA%\ninja.exe" set "PATH=%NINJA%;%PATH%"
set "CMAKE_DIR=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
if exist "%CMAKE_DIR%\cmake.exe" set "PATH=%CMAKE_DIR%;%PATH%"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CONFIGURATION_TYPES=Release || exit /b 1
cmake --build build --parallel || exit /b 1
echo.
echo Gotowe: %~dp0bin\Kierownica.exe
endlocal

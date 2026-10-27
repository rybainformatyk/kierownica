# Kompiluje i wgrywa firmware kierownicy do Arduino Due przez Programming Port.
#
#   wgraj_firmware.bat            - sam znajdzie port plytki
#   wgraj_firmware.bat COM5       - wybrany port
#   wgraj_firmware.bat -Kompiluj  - tylko kompilacja, bez wgrywania
#
# Przed wgraniem zamknij panel kierownicy (albo kliknij Rozlacz) - port COM
# moze miec otwarty tylko jeden program.

param(
    [string]$Port = "",
    [switch]$Kompiluj
)

$ErrorActionPreference = "Stop"
$fqbn = "arduino:sam:arduino_due_x_dbg"      # Arduino Due (Programming Port)
$sketch = $PSScriptRoot

function Fail($msg) {
    Write-Host ""
    Write-Host "BLAD: $msg" -ForegroundColor Red
    exit 1
}

# --- arduino-cli: ten z Arduino IDE 2, a jak go nie ma - z PATH ---
$cli = Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if (-not (Test-Path $cli)) {
    $found = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($found) { $cli = $found.Source }
    else { Fail "Nie znaleziono arduino-cli. Zainstaluj Arduino IDE 2 (arduino.cc/en/software)." }
}

Write-Host "Kompilacja firmware ($fqbn)..." -ForegroundColor Cyan
& $cli compile --fqbn $fqbn $sketch
if ($LASTEXITCODE -ne 0) {
    Fail "Kompilacja nie powiodla sie. Jesli brakuje rdzenia Due, zainstaluj w Arduino IDE: Narzedzia > Plytka > Menedzer plytek > 'Arduino SAM Boards'."
}
if ($Kompiluj) {
    Write-Host "Kompilacja OK." -ForegroundColor Green
    exit 0
}

# --- port: Programming Port Due to USB VID 2341 (lub 2A03 - arduino.org), PID 003D ---
if (-not $Port) {
    $ports = @(Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.PNPDeviceID -match "VID_(2341|2A03)&PID_003D" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { [regex]::Match($_.Name, "\((COM\d+)\)").Groups[1].Value })
    if ($ports.Count -eq 0) {
        Fail "Nie widze Arduino Due na Programming Port. Podlacz kabel do gniazda Programming Port (przy wtyku zasilania) albo podaj port recznie: wgraj_firmware.bat COM5"
    }
    if ($ports.Count -gt 1) {
        Fail ("Widze kilka plytek Due: " + ($ports -join ", ") + ". Podaj port recznie, np.: wgraj_firmware.bat " + $ports[0])
    }
    $Port = $ports[0]
}

Write-Host "Wgrywanie na $Port..." -ForegroundColor Cyan
& $cli upload --fqbn $fqbn -p $Port $sketch
if ($LASTEXITCODE -ne 0) {
    Fail "Wgrywanie nie powiodlo sie. Zamknij panel kierownicy (zajmuje port $Port) i sprobuj ponownie."
}

Write-Host ""
Write-Host "Gotowe. Firmware 2.1 wgrany." -ForegroundColor Green
Write-Host "Teraz podlacz drugi kabel do portu Native USB - Windows wykryje 'G29 Driving Force Racing Wheel'."
Write-Host "W panelu wybierz tryb wyjscia: Kierownica G29."

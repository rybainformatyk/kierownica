# Kompiluje i wgrywa firmware kierownicy.
#
#   wgraj_firmware.bat                          - sam rozpozna plytke i port
#   wgraj_firmware.bat COM5                     - wybrany port
#   wgraj_firmware.bat -Plytka uno              - wybrana plytka
#   wgraj_firmware.bat -Plytka esp32 -Port COM7
#   wgraj_firmware.bat -Plytka leonardo -Kompiluj   - tylko kompilacja
#
# Plytki: due, mega, uno, nano, leonardo, micro, esp32, esp32s3
#
# Przed wgraniem zamknij panel kierownicy (albo kliknij Rozlacz) - port COM
# moze miec otwarty tylko jeden program. Panel robi to sam, gdy wgrywanie
# uruchamia sie z zakladki Diagnostyka.

param(
    [string]$Port = "",
    [string]$Plytka = "",
    [switch]$Kompiluj
)

$ErrorActionPreference = "Stop"
$sketch = $PSScriptRoot
$esp32Index = "https://espressif.github.io/arduino-esp32/package_esp32_index.json"

function Fail($msg) {
    Write-Host ""
    Write-Host "BLAD: $msg" -ForegroundColor Red
    exit 1
}

# --- plytki: FQBN, rdzen i to, co plytka umie ---
$boards = [ordered]@{
    due      = @{ Name = "Arduino Due (Programming Port)"; Fqbn = "arduino:sam:arduino_due_x_dbg"; Core = "arduino:sam"; G29 = "native" }
    mega     = @{ Name = "Arduino Mega 2560"; Fqbn = "arduino:avr:mega:cpu=atmega2560"; Core = "arduino:avr"; G29 = "" }
    uno      = @{ Name = "Arduino Uno"; Fqbn = "arduino:avr:uno"; Core = "arduino:avr"; G29 = "" }
    nano     = @{ Name = "Arduino Nano"; Fqbn = "arduino:avr:nano:cpu=atmega328"; Core = "arduino:avr"; G29 = "" }
    leonardo = @{ Name = "Arduino Leonardo / Pro Micro"; Fqbn = "arduino:avr:leonardo"; Core = "arduino:avr"; G29 = "same" }
    micro    = @{ Name = "Arduino Micro"; Fqbn = "arduino:avr:micro"; Core = "arduino:avr"; G29 = "same" }
    esp32    = @{ Name = "ESP32 (DevKit)"; Fqbn = "esp32:esp32:esp32"; Core = "esp32:esp32"; G29 = "" }
    esp32s3  = @{ Name = "ESP32-S3 (DevKitC-1)"; Fqbn = "esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default"; Core = "esp32:esp32"; G29 = "same" }
}

# --- porty COM z identyfikatorem USB ---
function Get-SerialPorts {
    @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object {
            [pscustomobject]@{
                Port = [regex]::Match($_.Name, "\((COM\d+)\)").Groups[1].Value
                Name = $_.Name
                Id   = [string]$_.PNPDeviceID
            }
        })
}

# Plytka po VID/PID portu; "" gdy sie nie da (np. CH340 - Nano albo ESP32).
function Guess-Board($id) {
    if ($id -match "VID_(2341|2A03)&PID_003D") { return "due" }
    if ($id -match "VID_(2341|2A03)&PID_(0042|0010)") { return "mega" }
    if ($id -match "VID_(2341|2A03)&PID_(0043|0001|0243)") { return "uno" }
    if ($id -match "VID_(2341|2A03)&PID_(8036|0036)") { return "leonardo" }
    if ($id -match "VID_1B4F&PID_(9205|9206|9203|9204)") { return "leonardo" }
    if ($id -match "VID_(2341|2A03)&PID_(8037|0037)") { return "micro" }
    if ($id -match "VID_303A") { return "esp32s3" }
    return ""
}

# Czy port pasuje do plytki.
function Test-PortFor($board, $id) {
    switch ($board) {
        "due"      { return $id -match "VID_(2341|2A03)&PID_003D" }
        "mega"     { return $id -match "VID_(2341|2A03)&PID_(0042|0010)|VID_1A86|VID_0403" }
        "uno"      { return $id -match "VID_(2341|2A03)&PID_(0043|0001|0243)|VID_1A86|VID_0403" }
        "nano"     { return $id -match "VID_1A86|VID_0403|VID_(2341|2A03)&PID_(0043|0001)" }
        "leonardo" { return $id -match "VID_(2341|2A03)&PID_(8036|0036)|VID_1B4F|VID_046D&PID_C24F" }
        "micro"    { return $id -match "VID_(2341|2A03)&PID_(8037|0037)|VID_046D&PID_C24F" }
        "esp32"    { return $id -match "VID_10C4|VID_1A86|VID_0403" }
        "esp32s3"  { return $id -match "VID_303A|VID_046D&PID_C24F|VID_10C4|VID_1A86" }
    }
    return $false
}

$ports = Get-SerialPorts

# --- plytka ---
$Plytka = $Plytka.Trim().ToLower()
if (-not $Plytka) {
    $guesses = @()
    foreach ($p in $ports) {
        if ($Port -and $p.Port -ne $Port) { continue }
        $g = Guess-Board $p.Id
        if ($g) { $guesses += $g }
    }
    $guesses = @($guesses | Select-Object -Unique)
    if ($guesses.Count -eq 1) {
        $Plytka = $guesses[0]
        Write-Host "Rozpoznana plytka: $($boards[$Plytka].Name)" -ForegroundColor Cyan
    } else {
        Fail ("Nie umiem sam rozpoznac plytki. Podaj ja recznie, np.: wgraj_firmware.bat -Plytka uno`n" +
              "Dostepne: " + (($boards.Keys) -join ", "))
    }
}
if (-not $boards.Contains($Plytka)) {
    Fail ("Nieznana plytka '$Plytka'. Dostepne: " + (($boards.Keys) -join ", "))
}
$board = $boards[$Plytka]
$fqbn = $board.Fqbn

# --- arduino-cli: ten z Arduino IDE 2, a jak go nie ma - z PATH ---
$cli = Join-Path $env:LOCALAPPDATA "Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
if (-not (Test-Path $cli)) {
    $found = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($found) { $cli = $found.Source }
    else { Fail "Nie znaleziono arduino-cli. Zainstaluj Arduino IDE 2 (arduino.cc/en/software)." }
}

# --- rdzen plytki: doinstaluj, jesli go brakuje ---
$installed = (& $cli core list) -join "`n"
if ($installed -notmatch [regex]::Escape($board.Core)) {
    Write-Host "Instaluje obsluge plytki ($($board.Core)) - pierwszy raz to moze potrwac kilka minut..." -ForegroundColor Cyan
    $extra = @()
    if ($board.Core -eq "esp32:esp32") { $extra = @("--additional-urls", $esp32Index) }
    & $cli core update-index @extra
    & $cli core install $board.Core @extra
    if ($LASTEXITCODE -ne 0) { Fail "Nie udalo sie zainstalowac rdzenia $($board.Core). Sprawdz polaczenie z internetem." }
}

# --- port ---
if (-not $Port -and -not $Kompiluj) {
    $match = @($ports | Where-Object { Test-PortFor $Plytka $_.Id } | ForEach-Object { $_.Port } | Select-Object -Unique)
    if ($match.Count -eq 0) {
        $hint = ""
        if ($Plytka -eq "due") { $hint = " Podlacz kabel do gniazda Programming Port (przy wtyku zasilania)." }
        Fail "Nie widze plytki $($board.Name).$hint Podaj port recznie, np.: wgraj_firmware.bat -Plytka $Plytka -Port COM5"
    }
    if ($match.Count -gt 1) {
        Fail ("Pasuje kilka portow: " + ($match -join ", ") + ". Podaj port recznie, np.: wgraj_firmware.bat -Plytka $Plytka -Port " + $match[0])
    }
    $Port = $match[0]
}

# ESP32-S3 z firmware kierownicy ma port COM z TinyUSB - resetuje sie go
# wtedy "dotknieciem" 1200 bodow, a nie liniami DTR/RTS.
if ($Plytka -eq "esp32s3" -and $Port) {
    $info = $ports | Where-Object { $_.Port -eq $Port } | Select-Object -First 1
    if ($info -and $info.Id -match "VID_046D") { $fqbn += ",UploadMode=cdc" }
}

Write-Host "Kompilacja firmware dla $($board.Name) ($fqbn)..." -ForegroundColor Cyan
& $cli compile --fqbn $fqbn $sketch
if ($LASTEXITCODE -ne 0) {
    Fail "Kompilacja nie powiodla sie."
}
if ($Kompiluj) {
    Write-Host "Kompilacja OK." -ForegroundColor Green
    exit 0
}

Write-Host "Wgrywanie na $Port..." -ForegroundColor Cyan
& $cli upload --fqbn $fqbn -p $Port $sketch
$portInfo = $ports | Where-Object { $_.Port -eq $Port } | Select-Object -First 1
$cloneBridge = $portInfo -and $portInfo.Id -match "VID_1A86|VID_0403"
if ($LASTEXITCODE -ne 0 -and ($Plytka -eq "nano" -or ($Plytka -eq "uno" -and $cloneBridge))) {
    # klony Nano maja czesto stary bootloader (57600 bodow); panel nie odrozni
    # Nano od Uno - oba przedstawiaja sie jako KIEROWNICA-UNO
    Write-Host "Probuje jeszcze ze starym bootloaderem Nano..." -ForegroundColor Yellow
    $fqbn = "arduino:avr:nano:cpu=atmega328old"
    & $cli compile --fqbn $fqbn $sketch
    if ($LASTEXITCODE -eq 0) { & $cli upload --fqbn $fqbn -p $Port $sketch }
}
if ($LASTEXITCODE -ne 0) {
    $hint = "Zamknij panel kierownicy (zajmuje port $Port) i sprobuj ponownie."
    if ($Plytka -like "esp32*") { $hint += " Jesli dalej nie idzie: przytrzymaj przycisk BOOT na plytce podczas wgrywania." }
    Fail "Wgrywanie nie powiodlo sie. $hint"
}

Write-Host ""
Write-Host "Gotowe. Firmware wgrany na $($board.Name)." -ForegroundColor Green
switch ($board.G29) {
    "native" {
        Write-Host "Teraz podlacz drugi kabel do portu Native USB - Windows wykryje 'G29 Driving Force Racing Wheel'."
        Write-Host "W panelu wybierz tryb wyjscia: Kierownica G29."
    }
    "same" {
        Write-Host "Ten sam kabel jest laczem z panelem i kierownica G29 - Windows wykryje 'G29 Driving Force Racing Wheel'."
        Write-Host "W panelu wybierz tryb wyjscia: Kierownica G29."
    }
    default {
        Write-Host "Ta plytka nie ma natywnego USB, wiec nie udaje G29."
        Write-Host "W panelu wybierz tryb wyjscia vJoy albo pad Xbox 360."
    }
}

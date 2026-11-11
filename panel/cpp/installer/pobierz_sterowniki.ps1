# Pobiera instalatory sterownikow wbudowywane w instalator panelu
# (installer\deps\, poza repozytorium). Wolane z wydanie.bat.
#
# Kazdy plik musi miec dokladnie przypieta sume SHA-256 i wazny podpis
# cyfrowy wydawcy - podmieniony albo uszkodzony plik przerywa wydanie.
# Nowa wersja sterownika: zmien URL i sume ponizej oraz nazwy w kierownica.iss.

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$deps = Join-Path $PSScriptRoot "deps"
New-Item -ItemType Directory -Force $deps | Out-Null

$files = @(
    @{
        Name   = "ViGEmBus_1.22.0_x64_x86_arm64.exe"
        Url    = "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe"
        Sha256 = "89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a"
        Signer = "Nefarius Software Solutions"
    },
    @{
        Name   = "vJoySetup_v2.2.2.0_Win10_Win11.exe"
        Url    = "https://github.com/BrunnerInnovation/vJoy/releases/download/v2.2.2.0/vJoySetup_v2.2.2.0_Win10_Win11.exe"
        Sha256 = "ef569a3105cd301b89580f18f60c66b339e95296acf2c0dfcaf4b4bbf8ab68fe"
        Signer = "Brunner Elektronik AG"
    }
)

foreach ($f in $files) {
    $path = Join-Path $deps $f.Name
    if (-not (Test-Path $path) -or (Get-FileHash $path -Algorithm SHA256).Hash.ToLower() -ne $f.Sha256) {
        Write-Host "Pobieram $($f.Name)..."
        $tmp = "$path.tmp"
        Invoke-WebRequest $f.Url -OutFile $tmp -UseBasicParsing
        Move-Item -Force $tmp $path
    }
    $hash = (Get-FileHash $path -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $f.Sha256) {
        Remove-Item $path
        throw "$($f.Name): zla suma SHA-256 ($hash). Plik usuniety."
    }
    $sig = Get-AuthenticodeSignature $path
    if ($sig.Status -ne "Valid" -or $sig.SignerCertificate.Subject -notlike "*$($f.Signer)*") {
        throw "$($f.Name): brak waznego podpisu '$($f.Signer)' (status: $($sig.Status))."
    }
    Write-Host "OK  $($f.Name)  (podpis: $($f.Signer))"
}

# Kierownica DIY

Domowa kierownica do gier na **Arduino** (Due, Mega, Uno, Nano, Leonardo,
Micro) albo **ESP32 / ESP32-S3** z aplikacją na PC. Dzięki niej gry widzą
kierownicę jako **Logitech G29**, wirtualny joystick vJoy albo pad Xbox 360.
Kalibracja, mapowanie przycisków i Force Feedback są ustawiane w aplikacji,
więc zmiana ustawień nie wymaga ponownego wgrywania firmware.

## Struktura

```
firmware/
  kierownica/     firmware 3.0 dla wszystkich płytek (kierownica.ino, piny.h,
                  g29_*.h) i skrypt wgraj_firmware.bat
  skrzynia/       prototyp skrzyni biegów H (ArduinoJoystickLibrary, płytki z ATmega32U4)
panel/python/     aplikacja na PC (Python + Tkinter) - szczegóły w panel/python/README.md
panel/cpp/        ta sama aplikacja w C++ (Win32), wersja do wydania - panel/cpp/README.md
```

## Jak to działa

```
enkoder, pedały, przyciski ──► płytka ──(port COM, ramki z CRC8)──► panel na PC
                                  ▲                                     │
                                  └──────── gotowy raport G29 ◄─────────┘
                        USB płytki = "G29 Driving Force Racing Wheel" ──► gra
```

Płytka wysyła surowe odczyty ramkami binarnymi z CRC8. Panel je kalibruje
i mapuje, a potem, zależnie od trybu:

* **G29 (domyślny)**: odsyła płytce gotowy raport, a jej port USB
  przedstawia się Windowsowi jako prawdziwy G29 (`046D:C24F`). Działa
  w każdej grze z obsługą kierownic, także w Forzy. Nie wymaga sterowników.
  Tylko płytki z natywnym USB (tabela niżej).
* **vJoy**: kierownica DirectInput z Force Feedback, do symulatorów.
* **Pad Xbox 360 (ViGEm)**: do gier, które obsługują tylko XInput.

## Płytki

| Płytka | Łącze z panelem | G29 | Przyciski | Osie |
|---|---|---|---|---|
| Arduino Due | Programming Port; G29 na Native USB (2 kable) | tak | 32 | 6 |
| Arduino Leonardo / Micro / Pro Micro | jedno gniazdo USB | tak | 12 | 6 |
| ESP32-S3 (DevKitC-1) | gniazdo USB (OTG), nie UART | tak | 13 | 6 |
| Arduino Mega 2560 | USB | nie | 32 | 6 |
| Arduino Uno / Nano | USB | nie | 7 (10 przy 3 osiach) | 6 |
| ESP32 (DevKit, WROOM-32) | USB (CP210x / CH340) | nie | 10 | 5 |

Płytki bez G29 działają w trybach vJoy i pad. Piny każdej płytki opisuje
[firmware/kierownica/piny.h](firmware/kierownica/piny.h). Mega ma ten sam
układ pinów co Due:

| Element | Due / Mega |
|---|---|
| Enkoder kierownicy | 2, 3 |
| Pedały i osie dodatkowe (potencjometry) | A0–A5 |
| Przycisk 0 | 4 |
| Przyciski 1–31 | 22–52 |
| Mostek H do FFB (opcjonalnie) | 5 (EN), 6 (RPWM), 7 (LPWM) |

Due i ESP32 mają logikę 3,3 V - potencjometry zasilaj z 3,3 V, nie z 5 V.

## Szybki start

1. Pobierz `Kierownica-X.Y-instalator.exe` z
   [Releases](https://github.com/rybainformatyk/kierownica/releases/latest)
   i uruchom. Instalator ma w środku sterowniki **ViGEmBus** (tryb pada)
   i **vJoy** (tryby vJoy) i instaluje te, których brakuje - po jednym
   oknie zgody administratora. vJoy jest od razu ustawione pod panel
   (6 osi, 32 przyciski, POV, Force Feedback). Bez instalacji: wersja
   przenośna ZIP (sterowniki wtedy osobno).
2. Zainstaluj [Arduino IDE 2](https://www.arduino.cc/en/software) - panel
   używa jego `arduino-cli` do wgrywania firmware.
3. Podłącz płytkę, w zakładce **Diagnostyka** wybierz ją w polu **Płytka**
   i kliknij **Wgraj firmware**. Brakującą obsługę płytki (AVR, SAM, ESP32)
   skrypt doinstaluje sam.
4. Due: podłącz drugi kabel do **Native USB**. Leonardo, Micro i ESP32-S3
   pokażą G29 na tym samym kablu.
5. Wybierz tryb wyjścia u góry okna, w zakładce **G29** przypisz przyciski
   i kliknij **Zapisz**.

## Panel na co dzień

* **Praca w tle** - gry widzą kierownicę tylko wtedy, gdy panel działa, więc
  krzyżyk chowa go do zasobnika obok zegara. Menu ikony (prawy przycisk):
  tryb wyjścia, połączenie, autostart, Zakończ.
* **Autostart** - menu ikony, Diagnostyka albo zadanie w instalatorze; panel
  startuje od razu w tle.
* **Aktualizacje** - panel sam sprawdza wydania na GitHubie. Diagnostyka →
  Aktualizacje (albo menu ikony) → *Zaktualizuj*: pobiera, sprawdza sumę
  SHA-256, instaluje i uruchamia się ponownie z tymi samymi ustawieniami.
* Ustawienia: `%APPDATA%\KierownicaPanel\config.json` (wersja przenośna -
  obok programu).

## Budowanie i wydanie

```
panel\cpp\build.bat       program w panel\cpp\bin\
panel\cpp\wydanie.bat     testy + instalator ze sterownikami, ZIP, SHA256SUMS w panel\cpp\dist\
panel\cpp\opublikuj.bat   wydanie na GitHubie (gh) - panele zaktualizują się same
```

Szczegóły (Visual Studio 2022, Inno Setup 6, wersja w `src/version.h`):
[panel/cpp/README.md](panel/cpp/README.md).

Bez instalatora: zbuduj panel `panel/cpp/build.bat` albo uruchom wersję
Python (`panel/python`, Python 3.9+). Opis zakładek, trybów, sterowników
(vJoy, ViGEmBus) i protokołu jest w
[panel/python/README.md](panel/python/README.md).

## Licencja

Projekt jest na licencji [GNU GPL v3](LICENSE). Możesz go używać, zmieniać
i rozpowszechniać, także komercyjnie. Jeśli jednak rozpowszechniasz
program albo jego zmienioną wersję (np. sprzedajesz płytki z tym firmware
albo udostępniasz własną wersję aplikacji), musisz udostępnić kod źródłowy
na tej samej licencji.

### Fragmenty z innych projektów

* `panel/python/vigem/ViGEmClient.dll` pochodzi z pakietu
  [vgamepad](https://pypi.org/project/vgamepad/) (licencja MIT, zgodna
  z GPL, treść w `panel/python/vigem/LICENSE-vgamepad.txt`).
* Instalator zawiera niezmienione, podpisane przez autorów instalatory
  sterowników: [ViGEmBus](https://github.com/nefarius/ViGEmBus) 1.22.0
  (BSD-3-Clause, Nefarius Software Solutions) i
  [vJoy](https://github.com/BrunnerInnovation/vJoy) 2.2.2.0 (MIT,
  Brunner Elektronik AG / Shaul Eizikovich). Ich licencje dotyczą tylko
  tych sterowników.
* Deskryptor raportu G29 w `firmware/kierownica/g29_raport.h` (opis
  oryginalnego urządzenia Logitech G29, potrzebny do zgodności z grami)
  pochodzi z projektu
  [lgff_wheel_adapter](https://github.com/sonik-br/lgff_wheel_adapter),
  także na licencji GPL-3.0.

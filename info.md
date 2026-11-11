# Kierownica DIY - notatki z prac

## 2026-11-11 — Zrzuty ekranu panelu w README na GitHubie

**Co zrobione:** README w repozytorium pokazuje panel: duży zrzut Pulpitu pod opisem projektu i sekcję „Zrzuty ekranu” z zakładkami Kierownica, Pedały, G29, Pad i Force Feedback.

**Jak to działa:**
- Obrazy leżą w `docs/zrzuty/` (`pulpit.png`, `kierownica.png`, `pedaly.png`, `g29.png`, `pad.png`, `ffb.png`, razem ok. 1,7 MB, 1400×900).
- Generuje je `panel/cpp/tools/make_screenshots.py`. Skrypt uruchamia `Kierownica.exe --selftest <katalog tymczasowy>`: program nie otwiera okna, udaje podłączone Arduino Due z firmware 3.0 i rysuje każdą zakładkę do PNG. Potem skrypt kopiuje wybrane pliki pod stałymi nazwami (mapa `SHOTS`) i zapisuje je jako zoptymalizowane PNG bez strat (Pillow).

**Dlaczego tak:** zrzuty z selftestu są powtarzalne i nie zawierają danych z komputera użytkownika (ścieżek, nazw urządzeń). Można je odświeżyć jednym poleceniem po każdej zmianie wyglądu. Zdjęć samej kierownicy (sprzętu) w projekcie nie ma, więc README pokazuje tylko aplikację. Diagnostyka nie trafiła do galerii, bo w symulacji pokazuje zera (0 Hz, 0 ramek).

**Jak uruchomić / odświeżyć:**
```
panel\cpp\build.bat
py -3 panel\cpp\tools\make_screenshots.py                 # domyślnie bin\Kierownica.exe
py -3 panel\cpp\tools\make_screenshots.py --exe panel\cpp\dist\Kierownica.exe
```

**Uwagi i pułapki:**
- Symulowana płytka ma 0 Hz, więc na górnym pasku widać „COM9 · 0 Hz”.
- Nazwy plików selftestu zawierają polskie znaki i spacje („tab_2_Pedały.png”). Zmiana nazw zakładek w kodzie wymaga poprawienia mapy `SHOTS`.
- Zdjęcia sprzętu można dodać później do `docs/zrzuty/` i podlinkować w README tak samo.

## 2026-11-11 — Naprawa: nie dało się zmienić impulsów enkodera

### Co zrobione

Wpisana w zakładce Kierownica liczba „Impulsy na obrót kierownicy” zawsze wracała do starej wartości. Teraz zatwierdza ją Enter, Tab, kliknięcie obok albo „Zapisz”, a Esc cofa wpis.

### Jak to działa

- Przyczyna: `WheelPage::refresh()` co klatkę wpisuje do pola wartość z configu, gdy pole nie ma fokusu. Enter tylko zdejmował fokus i nic nie zatwierdzał. „Zapisz” przegrywał wyścig: `WM_LBUTTONDOWN` zdejmuje fokus, odświeżenie (~16 ms) przywraca starą liczbę, a przycisk działa dopiero na puszczeniu myszy i zapisywał tę starą liczbę.
- Naprawa w `ui/widgets.*`: `Entry` dostał callback `on_commit`, wołany w `focus_changed(false)`, gdy tekst różni się od tego sprzed edycji (ten sam wzorzec co `Slider::focus_changed`). Enter i Tab zdejmują fokus, więc zatwierdzają. Esc przywraca `before_edit_`.
- `WheelPage::apply_cpr(text)` (`ui/page_setup.cpp`): przy błędnym wpisie pokazuje komunikat w oknie i cofa pole, bez okna modalnego, bo utrata fokusu dzieje się w trakcie kliknięcia myszą i MessageBox popsułby przechwycenie myszy. Ta sama wartość co w configu = nic nie robi (klik „Zapisz” po automatycznym zatwierdzeniu).
- Dalsza ścieżka była poprawna: `apply_config()` → `publish_config()` → `SteerProcessor::process()` czyta `counts_per_rev` w każdej ramce, a `push_board_config()` wysyła płytce nowy próg miękkiego ogranicznika.

### Jak przetestować

`bin\Kierownica.exe --selftest katalog` — sprawdzenia „pole: …” i „impulsy enkodera: …” w `selftest.log`.

### Uwagi i pułapki

- `Entry` jest używane tylko w tym miejscu. Nowe pola tekstowe dostają zatwierdzanie przez trzeci argument konstruktora.
- Wydane jako [v3.2.1](https://github.com/rybainformatyk/kierownica/releases/tag/v3.2.1) — panele 3.2 zaktualizują się same.
- Przy okazji: `APP_VERSION_TEXT` (`src/version.h`) ma numer poprawki, gdy jest różny od 0 („3.2.1”). Wcześniej byłoby to „3.2”, więc 3.2.1 widziałaby wydanie 3.2.1 jako nowsze i aktualizowała się w kółko. Pilnuje tego test w `core_tests` (`APP_VERSION_TEXT` == `APP_VERSION_FULL`).


## 2026-11-11 — Publikacja wersji 3.2 na GitHubie

### Co zrobione

Zmiany wersji 3.2 trafiły do `main` przez PR #5 (gałąź `wersja-cpp`). Wydanie [v3.2](https://github.com/rybainformatyk/kierownica/releases/tag/v3.2) zawiera instalator ze sterownikami, wersję przenośną ZIP, `Kierownica.exe` i `SHA256SUMS.txt`. README (główne i `panel/cpp`) opisuje instalator ze sterownikami, pracę w tle, aktualizacje i licencje dołączonych sterowników (ViGEmBus BSD-3-Clause, vJoy MIT).

### Jak to działa

- Schemat repo: praca na `wersja-cpp`, PR do `main` scalany przez merge commit, tag `vX.Y` na commicie scalenia.
- Publikacja: `panel\cpp\wydanie.bat`, potem `panel\cpp\opublikuj.bat` (albo ręcznie `gh release create vX.Y dist\... --target main --notes-file installer\NOWOSCI.md`).
- `SHA256SUMS.txt` ma końce linii LF, bo `sha256sum -c` na Linuksie i w Git Bash nie radzi sobie z CRLF. Parser w panelu przyjmuje oba.

### Dlaczego tak

- Jest `SHA256SUMS.txt` i osobny `Kierownica.exe` — bez nich panele nie zaktualizują się same (instalacja wymaga sumy, wersja przenośna podmienia sam exe).

### Jak przetestować

```
gh release download v3.2 -R rybainformatyk/kierownica -D rel && cd rel && sha256sum -c SHA256SUMS.txt
set KIEROWNICA_LIVE=1 && panel\cpp\bin\tests\core_tests.exe    # "mozna instalowac: tak"
```

### Uwagi i pułapki

- Panele 3.1 nie mają aktualizatora — użytkownicy 3.1 muszą raz ręcznie pobrać 3.2. Od 3.2 aktualizacje są automatyczne.
- Pierwsza prawdziwa aktualizacja z panelu (3.2 → 3.3) dopiero przetestuje cały mechanizm end-to-end.


## 2026-11-11 — Instalator ze sterownikami, aktualizacje z panelu, ikony w Windowsie

### Co zrobione

Instalator (`Kierownica-3.2-instalator.exe`, 10,5 MB) zawiera ViGEmBus 1.22.0 i vJoy 2.2.2.0. Instaluje brakujące sterowniki po jednej zgodzie UAC i od razu konfiguruje vJoy pod panel. Panel sam sprawdza wydania na GitHubie (`rybainformatyk/kierownica`), a potem pobiera, weryfikuje SHA-256, instaluje i uruchamia się ponownie. Okno C++ ma ikony zależne od DPI. Wersja Python ma nowe logo i własny identyfikator na pasku zadań.

### Jak to działa

- **Sterowniki**: `panel/cpp/installer/pobierz_sterowniki.ps1` pobiera instalatory do `installer/deps/` (poza gitem), sprawdza przypiętą sumę SHA-256 i ważny podpis Authenticode (Nefarius Software Solutions, Brunner Elektronik AG). W `kierownica.iss` zadania `vigem`/`vjoy` pokazują się tylko, gdy brakuje usługi `ViGEmBus`/`vjoy` w `HKLM\SYSTEM\CurrentControlSet\Services`. Po instalacji `InstallDrivers()` tworzy `{tmp}\sterowniki.cmd` i uruchamia go przez `ShellExec('runas')`: ViGEmBus `/exenoui /qn /norestart`, vJoy `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-`, potem `vJoyConfig 1 -f -a x y z rx ry rz -b 32 -p 1 -e All`. Kody wyjścia trafiają do logu; 3010 wymusza restart na końcu.
- **Aktualizacje**: `src/updater.h/.cpp` (WinHTTP, BCrypt SHA-256, parser JSON panelu). `Updater::check()` i `download()` działają w wątku tła, a stan (`State`, z licznikiem `serial`) czyta `PanelApp::poll_updater()` w każdej klatce odświeżania. Rodzaj instalacji: `unins000.exe` obok exe → instalator; brak → wersja przenośna.
  - Instalator: `/SILENT /SUPPRESSMSGBOXES /NORESTART /UPDATE=1|tray`. `InitializeSetup` czeka do 15 s na zwolnienie mutexu `Local\KierownicaPanel`, a wpis `[Run]` z `Check: IsUpdate` uruchamia `Kierownica.exe --po-aktualizacji [--minimized]`.
  - Wersja przenośna: działający exe zmienia nazwę na `Kierownica.exe.old`, nowy trafia na jego miejsce i jest uruchamiany.
  - `--po-aktualizacji` (`main.cpp`): czeka do 15 s na mutex, a `cleanup_after_update()` usuwa `.old` i `%LOCALAPPDATA%\Kierownica\aktualizacje`.
- UI: karta **Aktualizacje** w Diagnostyce (`DiagPage::set_update`), pozycja „Zaktualizuj do wersji X” w menu ikony, dymek przy panelu w zasobniku, przełącznik `ui.updates` (domyślnie tak, dodany także w Pythonie dla zgodności config.json).
- Bezpieczeństwo: pobierane są tylko adresy `https://github.com/rybainformatyk/kierownica/releases/download/`. Bez `SHA256SUMS.txt` w wydaniu nic się nie instaluje samo, przycisk otwiera stronę wydania.
- Publikacja: `wydanie.bat` czyści `dist\` i robi instalator, ZIP, `Kierownica.exe` i `SHA256SUMS.txt`. `opublikuj.bat` wywołuje `gh release create vX.Y` z `installer\NOWOSCI.md` jako opisem.
- Ikony: `PanelApp::apply_window_icons()` ustawia `WM_SETICON` w rozmiarach z `GetSystemMetricsForDpi` (też po `WM_DPICHANGED`). Instalator ma `ChangesAssociations=yes`, czyli odświeża ikony w Eksploratorze. Python: `wheelbridge/ui/res/logo_*.png` (generuje je `make_icon.py`) i `SetCurrentProcessExplicitAppUserModelID("KierownicaDIY.PanelPython")` w `kierownica.py`.

### Dlaczego tak

- Sterowniki wbudowane zamiast pobieranych przy instalacji: instalacja działa offline, a podpis i sumę sprawdza się raz, przy budowaniu wydania. Koszt to ok. 9 MB więcej.
- vJoy z forka BrunnerInnovation 2.2.2.0, bo oryginał z SourceForge (2.1.9.1, 2019) ma problemy na Windows 11. API DLL jest to samo, a panel szuka `vJoyInterface.dll` niezależnie od wersji.
- Jeden skrypt `.cmd` jako administrator: jedno okno UAC zamiast trzech. Instalator panelu może dalej działać bez uprawnień administratora.
- Aktualizacja przez instalator (a nie podmianę exe) dla wersji zainstalowanej: wpis w „Aplikacjach” i deinstalator mają właściwą wersję.
- Własne sprawdzanie mutexu w `InitializeSetup` zamiast `AppMutex`: przy aktualizacji panel zamyka się dopiero po uruchomieniu instalatora, więc instalator musi na niego poczekać.

### Jak uruchomić / przetestować

```
cd panel\cpp
wydanie.bat                                   # dist\ ze sterownikami
set KIEROWNICA_LIVE=1 && bin\tests\core_tests.exe   # test z prawdziwym GitHubem
opublikuj.bat                                 # publikacja wydania (gh)
```

Sprawdzone 2026-11-11: unit-testy aktualizatora (wersje, JSON wydania, SHA256SUMS, zaufane URL, SHA-256 wektora „abc”, rodzaj instalacji). Na żywo: API zwraca v3.1, pobranie `Kierownica.exe` z v3.1 przez przekierowanie GitHuba daje sumę zgodną z `gh release download`. Przeszły też selftest okna, test wzorcowy (51 015 sprawdzeń) i pytest Pythona (58).

### Uwagi i pułapki

- **Nie przetestowane w praktyce**: instalacja sterowników przez instalator (wymaga UAC, a ViGEmBus był już zainstalowany) i pełna aktualizacja end-to-end. Do tego potrzebne jest wydanie nowsze niż 3.2 z `SHA256SUMS.txt`, a pierwszym takim będzie opublikowana 3.2 dla przyszłej 3.3. Instalatora nie uruchamiałem, bo działający panel trzymał mutex.
- Parametry cichej instalacji ViGEmBus (`/exenoui /qn`) pochodzą z dokumentacji Advanced Installer i nie były sprawdzone na tym instalatorze.
- Wydanie v3.1 na GitHubie nie ma `SHA256SUMS.txt`, więc panel 3.2 tylko je pokaże (i tak jest starsze). Automatyczne aktualizacje zadziałają od pierwszego wydania opublikowanego przez `opublikuj.bat`.
- Jeśli UAC zatwierdza inne konto administratora, skrypt `.cmd` leży w `%TEMP%` instalującego użytkownika i może nie być dla tego konta dostępny.
- Build przy działającym panelu kończy się `LNK1104`. Pomaga przemianowanie `bin\Kierownica.exe` na `.old` (działający exe można przemianować).
- Exe i instalator nie są podpisane — SmartScreen ostrzega, a aktualizacja opiera się tylko na SHA-256 z tego samego wydania (chroni przed uszkodzeniem pliku, nie przed przejęciem repozytorium).


## 2026-11-11 — Nowe logo programu

### Co zrobione

Nowe logo: kafelek w stylu Windows 11 (zaokrąglony kwadrat, granatowy gradient z błękitną poświatą) ze sportową kierownicą w kształcie litery D (ścięty dół), z pomarańczowym znacznikiem środka i piastą z akcentem. Logo jest w pliku exe, w instalatorze i w lewym górnym rogu okna. Zasobnik ma osobne ikony: samą kierownicę, niebieską przy połączonej płytce i szarą bez niej.

### Jak to działa

- `panel/cpp/tools/make_icon.py` (Pillow) rysuje wszystko do `src/res/`: `kierownica.ico` (program, 16–256 px), `tray_on.ico`, `tray_off.ico` i `kierownica_256.png` (instalator). `--preview plik.png` tworzy arkusz podglądu na ciemnym i jasnym tle oraz powiększenie x8 małych rozmiarów.
- Każdy rozmiar jest rysowany osobno (nadpróbkowanie x4). Do 24 px rysunek jest uproszczony: grubszy wieniec, pełna, mniejsza piasta, wąskie szprychy, żeby otwory zostały widoczne. Funkcja `fit()` dopasowuje kierownicę do pola (zasobnik: całe pole, kafelek: 70%, małe kafelki: 78%).
- Zasoby: `IDI_APP=1`, `IDI_TRAY_ON=2`, `IDI_TRAY_OFF=3` (`embedded_ids.h`, `kierownica.rc`).
- Logo w oknie: `Gfx::icon(id, rect)` (`ui/gfx.cpp`) wczytuje ikonę przez `LoadIconWithScaleDown` w rozmiarze dla bieżącego DPI, przemnaża kolory przez alfę i trzyma bitmapę D2D w pamięci podręcznej (`icon/<id>/<px>`). `Sidebar::paint` rysuje ją w 34 DIP.

### Dlaczego tak

- Osobne ikony zasobnika bez kafelka: w 16 px kafelek zabierał miejsce kierownicy. Zasobnik ma mocniejszy błękit, bo pasek zadań bywa jasny.
- Logo w oknie z tej samej ikony co exe, a nie rysowane wektorowo osobno: jedno źródło wyglądu.
- Kształt D i pomarańczowy znacznik: od razu kojarzą się z kierownicą wyścigową i są czytelne nawet w 20 px.

### Jak uruchomić / przetestować

```
cd panel\cpp
py -3 tools\make_icon.py                  # ikony do src\res
py -3 tools\make_icon.py --preview p.png  # tylko podgląd (p.png i p_male.png)
build.bat && wydanie.bat
```

### Uwagi i pułapki

- Po zmianie ikon Eksplorator może pokazywać starą ikonę z pamięci podręcznej. Pomaga `ie4uinit.exe -show` albo restart Eksploratora.
- Build może zgłosić `LNK1104` (nie da się nadpisać `bin\Kierownica.exe`), gdy Nextcloud akurat synchronizuje plik. Wystarczy ponowić.


## 2026-11-11 — Wiele płytek, zasobnik, autostart i wydanie (panel 3.2, firmware 3.0)

### Co zrobione

Firmware 3.0 kompiluje się na Arduino Due, Mega 2560, Uno, Nano, Leonardo, Micro, ESP32 i ESP32-S3. G29 działa na płytkach z natywnym USB: Due, Leonardo/Micro i ESP32-S3. Panel C++ (wersja 3.2) siedzi w zasobniku i działa w tle. Autostart jest dostępny w menu ikony, w Diagnostyce i w instalatorze. `wydanie.bat` robi instalator Inno Setup, wersję przenośną ZIP i sumy SHA-256.

### Jak to działa

**Firmware** (`firmware/kierownica/`):
- `piny.h` — jedyne miejsce z różnicami płytek: piny enkodera, lista przycisków (max 32), osie (`PIN_NONE` = brak), piny mostka FFB, rozdzielczość ADC, port łącza, rodzaj G29. Płytka wybierana przez makra kompilatora (`ARDUINO_SAM_DUE`, `__AVR_ATmega2560__`, `__AVR_ATmega32U4__`, `__AVR_ATmega328P__`, `CONFIG_IDF_TARGET_ESP32S3/ESP32`).
- `kierownica.ino` — wspólna logika (protokół, debounce, filtr IIR, FFB). ADC zawsze skalowane do 0..4095. AVR czyta 2 osie na tik 1 ms (analogRead ~110 µs) z krótszym filtrem. Enkoder czyta rejestry bezpośrednio (PIO w SAM, `portInputRegister` w AVR, `GPIO_IN_REG` w ESP32, ISR w IRAM). 32-bitowy licznik w AVR jest czytany atomowo. Tablica CRC jest w PROGMEM.
- `g29_raport.h` — wspólny deskryptor G29 (046D:C24F). `g29_usb.h` — PluggableUSB dla Due i 32U4 (deskryptor urządzenia podmieniany w `getDescriptor`, nie przez build.vid). `g29_tinyusb.h` — ESP32-S3: `USBHIDDevice` i `tud_hid_n_report` bez czekania. VID/PID są ustawiane przed `USB.begin()`, dlatego trzeba `CDCOnBoot=default`, a port COM to własny `USBCDC wheelCdc`.
- HELLO: nazwa `KIEROWNICA-DUE/-MEGA/-UNO/-LEONARDO/-ESP32/-ESP32S3`, liczba przycisków z `piny.h`, `CAP_NATIVE_USB` gdy łącze idzie natywnym USB.
- `wgraj_firmware.ps1 -Plytka <due|mega|uno|nano|leonardo|micro|esp32|esp32s3> -Port COMn [-Kompiluj]` — FQBN dla płytki, rozpoznanie płytki i portu po VID/PID, doinstalowanie brakującego rdzenia (dla ESP32 z URL Espressif). Uno/Nano na CH340/FTDI: druga próba ze starym bootloaderem. ESP32-S3 z naszym firmware (VID 046D): `UploadMode=cdc`.

**Panel C++** (`panel/cpp/`):
- `protocol.h`: `FW_LATEST` 3.0, tabela `BOARDS` (klucz, nazwa, nazwa w HELLO), `HelloInfo::board()`, `g29_unsupported()` (aktualny firmware bez G29 = płytka bez natywnego USB), `g29_same_cable()`.
- `link.cpp`: `wants_dtr()` — DTR podniesione dla portów `&MI_` / VID 303A (Leonardo i ESP32-S3 bez DTR milczą). `probe_timeout()` — 2,5 s dla CH340/CP210x/FTDI (reset przy otwarciu). Lista „wygląda na Arduino” obejmuje CP210x, FTDI, Espressif, SparkFun.
- Silnik i UI: stan G29 `no_board` („płytka bez USB”, podpowiedź: wybierz vJoy/pad), komunikaty „odłącz i podłącz kabel” dla G29 na jednym kablu, nazwa płytki w Diagnostyce, na Pulpicie i w popoverze łącza.
- Diagnostyka: pole **Płytka** (`link.board` w config.json, „” = według HELLO). „Wgraj firmware” przekazuje `-Plytka` i `-Port`. Przełącznik „Działaj w tle po zamknięciu okna” (`ui.tray`, domyślnie tak).
- Zasobnik: `src/ui/tray.h/.cpp` (Shell_NotifyIcon v4, ponowne dodanie po `TaskbarCreated`), logika w `ui/app.cpp`: `request_close()` (krzyżyk/Alt+F4 → zasobnik), `hide_to_tray()`, `show_window()`, `show_tray_menu()`, `refresh_tray()`. `--minimized` przy włączonym zasobniku startuje bez okna. `WM_ENDSESSION` zapisuje ustawienia i zatrzymuje silnik. Druga kopia programu wysyła komunikat `KierownicaPanel.Show` (`winsys::bring_to_front`).
- `winsys`: `exe_path()`, `autostart_points_here()`, `autostart_enable()`, `show_message()`.
- Ustawienia: obok exe tylko przy `portable.txt` albo istniejącym `config.json`. W pozostałych przypadkach `%APPDATA%\KierownicaPanel\config.json`.
- Wydanie: `src/version.h` (jedno źródło wersji), `kierownica.rc` (ikony `src/res/*.ico`, VERSIONINFO, nowe pliki firmware), `installer/kierownica.iss`, `installer/CZYTAJ.txt`, `wydanie.bat`, `tools/make_icon.py` (Pillow).
- Python (`panel/python/wheelbridge/config.py`) dostał te same klucze `link.board` i `ui.tray`, żeby wspólny config.json i test wzorcowy się zgadzały.

### Dlaczego tak

- Jeden szkic + `piny.h` zamiast osobnych szkiców: protokół i FFB są wspólne, różnice to tylko piny i USB. Odrzucone: osobny firmware na płytkę (rozjeżdżałyby się poprawki).
- G29 na Leonardo przez podmianę deskryptora w PluggableUSB, a nie `--build-property build.vid/pid`. Cytowanie `usb_product` z PowerShell jest kruche, a podmiana już działała na Due.
- ESP32-S3 przez `tud_hid_n_report`, bo `USBHID::SendReport` czeka na hosta (zablokowałby pętlę i watchdog FFB).
- DTR wybierany po identyfikatorze portu, a nie zawsze podniesiony: podniesione DTR resetuje Due/Uno przez 16U2.
- Stan „płytka bez USB” osobno od „stary firmware”: inaczej panel kazałby wgrać firmware, co dla Uno niczego nie zmienia.
- Instalator domyślnie per-user (bez UAC), ustawienia w APPDATA — aktualizacja i deinstalacja ich nie ruszają. Wersja w jednym pliku (`version.h`), instalator czyta ją z exe.

### Jak uruchomić / wgrać / przetestować

```
cd panel\cpp
build.bat                                  # bin\Kierownica.exe
bin\tests\core_tests.exe                   # 152 sprawdzenia (w tym płytki, DTR)
bin\Kierownica.exe --selftest katalog      # PNG wszystkich zakładek + scenariusz Uno
py -3 -B tests\make_golden.py golden.json && bin\tests\core_tests.exe golden.json
wydanie.bat                                # dist\: instalator, ZIP, SHA256SUMS.txt
py -3 tools\make_icon.py                   # ikony, gdy zmienia się wygląd

firmware\kierownica\wgraj_firmware.bat -Plytka uno -Kompiluj   # sama kompilacja
firmware\kierownica\wgraj_firmware.bat -Plytka esp32 -Port COM7
```

Sprawdzone 2026-11-11: kompilacja firmware dla due, mega, uno, nano (oba bootloadery), leonardo, micro, esp32, esp32s3 (OTG i HW CDC); core_tests, test wzorcowy (50 990 sprawdzeń), pytest Pythona (58), selftest okna; cicha instalacja i deinstalacja instalatora.

### Uwagi i pułapki

- **Nie testowane na sprzęcie** (nie było płytek): G29 na Leonardo/Micro i ESP32-S3, odczyty na Mega/Uno/ESP32, wgrywanie przez skrypt. Due działa jak wcześniej (ten sam kod USB), ale też warto sprawdzić po wgraniu 3.0.
- Leonardo / ESP32-S3 z VID Logitecha: jeśli jest zainstalowany Logitech G HUB, jego sterownik może przejąć całe urządzenie złożone i zepsuć port COM. Na Due problemu nie ma (osobne gniazdo).
- Na Leonardo i ESP32-S3 G29 nie da się ukryć w trybach vJoy/pad — gry widzą dwa urządzenia.
- Pro Micro nie ma pinów 11-13 ani A4-A5 — te przyciski i osie są nieużywane. ESP32 WROVER: GPIO 16/17 zajmuje PSRAM. ESP32-S3 N8R8: GPIO 33-37 zajęte.
- Uno: tylko 7 przycisków; `#define UNO_OSIE 3` daje 10 przycisków kosztem osi aux.
- W środowisku z `NoDefaultCurrentDirectoryInExePath` cmd nie uruchamia `.bat` z bieżącego katalogu — skrypty wywołują się pełną ścieżką (`%~dp0`).
- Dotychczasowy `bin\config.json` zostaje używany. Nowy `bin\` bez configu zapisze go w `%APPDATA%\KierownicaPanel`.
- Inno Setup 6 został zainstalowany przez `winget install JRSoftware.InnoSetup --scope user` (potrzebny tylko do budowy instalatora).
- Instalator i exe nie są podpisane cyfrowo — SmartScreen pokaże ostrzeżenie „nieznany wydawca”.

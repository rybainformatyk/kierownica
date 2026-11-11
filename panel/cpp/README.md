# Panel kierownicy - wersja C++

Ta sama aplikacja co w `../python`, napisana w C++20 na czystym Win32 i
Direct2D. Wynik to jeden plik `Kierownica.exe` bez zależności: nie trzeba
Pythona, pyserial ani Pillow. Opis trybów i protokołu jest w
[`../python/README.md`](../python/README.md) - dotyczy obu wersji. Interfejs tej
wersji jest przebudowany i wygląda inaczej niż w Pythonie.

## Interfejs

* Własna ramka okna: nawigacja boczna (poniżej 1320 px zwija się do ikon), pasek
  u góry ze stanem łącza, wyborem trybu i zapisem. Działa przyciąganie okna do
  krawędzi i Snap Layouts.
* Pulpit: kokpit na żywo, wykres skrętu, momentu i pedałów z ostatnich 10 sekund,
  stan systemu z przyciskami naprawy.
* Kierownica i Pedały: krzywą i martwe strefy można przeciągać myszą na wykresie.
* Przyciski: siatka 32 kafelków i panel szczegółów. Wciśnięcie przycisku na
  kierownicy wybiera jego kafelek.
* Suwaki: kliknięcie wartości pozwala ją wpisać, podwójne kliknięcie wraca do
  domyślnej, strzałki zmieniają o krok.
* Skróty: `Ctrl+S` zapis, `Ctrl+1..8` zakładki, `Esc` przerywa naukę przycisku.

## Praca w tle (zasobnik)

Gry widzą kierownicę tylko wtedy, gdy panel działa, dlatego panel siedzi
w zasobniku obok zegara:

* Krzyżyk i `Alt+F4` chowają okno do zasobnika (wyłącza się to w Diagnostyce
  albo w menu ikony). Pierwszy raz w sesji pokazuje się dymek z wyjaśnieniem.
* Klik ikony otwiera / chowa okno. Prawy przycisk daje menu: płytka i port,
  Połącz / Rozłącz, tryb wyjścia, autostart, zachowanie krzyżyka i Zakończ.
* Ikona jest niebieska przy połączonej płytce, szara bez niej. Podpowiedź
  pokazuje płytkę, port i stan wyjścia.
* Gdy płytka zniknie, a okno jest schowane, panel pokazuje dymek.
* Autostart (`--minimized`) uruchamia panel od razu w zasobniku. Ponowne
  uruchomienie programu pokazuje działające okno zamiast drugiej kopii.
* Wylogowanie i zamknięcie Windowsa zapisuje ustawienia i wyłącza FFB.

## Płytki

Firmware 3.0 (`../../firmware/kierownica`) działa na Due, Mega 2560, Uno,
Nano, Leonardo, Micro, ESP32 i ESP32-S3. Panel rozpoznaje płytkę po nazwie
w HELLO (`KIEROWNICA-UNO` itd.) i po bitach możliwości:

* płytki bez natywnego USB (Uno, Nano, Mega, ESP32) mają w trybie G29
  komunikat "płytka bez USB" zamiast "stary firmware", a tryby G29 są
  wyszarzone w menu ikony,
* Leonardo i ESP32-S3 mają G29 na tym samym kablu co łącze - nie da się jej
  ukryć w trybach vJoy / pad (informacja w dzienniku),
* porty natywnego USB (`&MI_` w identyfikatorze, Espressif `303A`) są
  otwierane z podniesionym DTR - bez tego Leonardo i ESP32-S3 nic nie
  wysyłają. Mostki USB-UART dalej mają DTR opuszczone (inaczej reset Due/Uno),
* na portach CH340 / CP210x / FTDI panel czeka na HELLO 2,5 s zamiast 1,2 s
  (klony resetują się przy otwarciu portu).

**Wgraj firmware** (Diagnostyka) bierze płytkę z pola **Płytka**, a przy
"Automatycznie" - z HELLO podłączonej płytki, i wywołuje
`wgraj_firmware.bat -Plytka <klucz> -Port COMn`.

## Budowanie

Potrzebne Visual Studio 2022 albo Build Tools z komponentem C++ (CMake i Ninja
są w pakiecie).

```
build.bat
```

Wynik trafia do `bin\`: `Kierownica.exe` i `tests\core_tests.exe`. Do działania
panelu wystarczy sam `Kierownica.exe` - biblioteka pada i firmware są w nim
wbudowane i rozpakowują się do `%LOCALAPPDATA%\Kierownica`.

Nie da się wbudować sterowników systemowych: tryb vJoy wymaga instalacji vJoy,
tryb pada instalacji ViGEmBus. Wgrywanie firmware wymaga Arduino IDE 2.

## Wydanie

```
wydanie.bat
```

Buduje program, uruchamia `core_tests` i selftest okna (przy błędzie
przerywa), a potem w `dist\` zapisuje:

* `Kierownica-X.Y-instalator.exe` - instalator Inno Setup 6
  (`installer\kierownica.iss`): domyślnie dla bieżącego użytkownika bez okna
  administratora, opcjonalnie dla wszystkich; zadania "autostart" i "skrót na
  pulpicie"; czeka na zamknięcie działającego panelu; deinstalacja usuwa
  autostart (jeśli wskazuje tę instalację) i `%LOCALAPPDATA%\Kierownica`,
  ale zostawia ustawienia,
* `Kierownica-X.Y-przenosna.zip` - exe + `portable.txt` (ustawienia obok
  programu), `CZYTAJ.txt`, licencja,
* `Kierownica.exe` - sam program, z niego aktualizuje się wersja przenośna,
* `SHA256SUMS.txt`.

Inno Setup: `winget install JRSoftware.InnoSetup` (bez niego powstanie sam ZIP).

### Sterowniki w instalatorze

`installer\pobierz_sterowniki.ps1` (wołany z `wydanie.bat`) pobiera do
`installer\deps\` (poza gitem) ViGEmBus 1.22.0 i vJoy 2.2.2.0 (fork
BrunnerInnovation, podpisany, Windows 10/11). Każdy plik ma przypiętą sumę
SHA-256 i musi mieć ważny podpis wydawcy - inaczej wydanie się przerywa.

Instalator pokazuje zadania tylko dla brakujących sterowników (usługa
`ViGEmBus` / `vjoy` w rejestrze). Po skopiowaniu plików robi jeden skrypt
`sterowniki.cmd` i uruchamia go jako administrator (jedno okno UAC):
ViGEmBus `/exenoui /qn /norestart`, vJoy `/VERYSILENT /NORESTART`, potem
`vJoyConfig 1 -f -a x y z rx ry rz -b 32 -p 1 -e All`. Kod 3010 = restart
na końcu instalacji. Aktualizacja z panelu (`/UPDATE`) sterowników nie rusza.

### Publikacja i aktualizacje w panelu

```
opublikuj.bat     # gh release create vX.Y z dist\ i opisem z installer\NOWOSCI.md
```

Panel (`src/updater.*`) chwilę po starcie pyta
`api.github.com/repos/rybainformatyk/kierownica/releases/latest` (wyłącza to
Diagnostyka → Aktualizacje → "Sprawdzaj przy starcie"). Gdy wydanie jest
nowsze niż `src/version.h`:

* instalacja z instalatora (obok exe jest `unins000.exe`): pobiera
  `Kierownica-X.Y-instalator.exe`, sprawdza SHA-256 z `SHA256SUMS.txt`,
  uruchamia go z `/SILENT /UPDATE=1` (`=tray`, gdy panel był w zasobniku)
  i się zamyka; instalator czeka na zamknięcie panelu i uruchamia go z
  `--po-aktualizacji`,
* wersja przenośna: pobiera `Kierownica.exe`, przemianowuje działający exe
  na `Kierownica.exe.old`, kładzie nowy na jego miejsce i go uruchamia,
* `--po-aktualizacji`: nowa wersja czeka do 15 s na zamknięcie starej,
  usuwa `.old` i pobrane pliki (`%LOCALAPPDATA%\Kierownica\aktualizacje`).

Pobierane są tylko adresy `https://github.com/rybainformatyk/kierownica/releases/download/...`.
Wydanie bez `SHA256SUMS.txt` (np. 3.1) panel tylko pokazuje - przycisk
otwiera stronę wydania. Test na żywo z GitHubem:
`set KIEROWNICA_LIVE=1 && bin\tests\core_tests.exe`.

Nowa wersja: zmień `src/version.h` - z niej biorą się tytuł okna, zasób
VERSIONINFO (właściwości pliku) i numer w instalatorze. Ikony programu
(`src/res/*.ico`) rysuje `py -3 tools\make_icon.py`.

## Uruchomienie

```
bin\Kierownica.exe
bin\Kierownica.exe --minimized            start w zasobniku (autostart)
bin\Kierownica.exe --config plik.json     inny plik ustawień
```

Ustawienia (`config.json`, ten sam format co w wersji Python - plik można
przenosić między wersjami) leżą:

* obok `Kierownica.exe`, gdy jest tam plik `portable.txt` (wersja przenośna)
  albo `config.json` już tam jest (np. dotychczasowy `bin\config.json`),
* w pozostałych przypadkach w `%APPDATA%\KierownicaPanel\config.json` -
  tak działa zainstalowany program.

Ścieżka jest widoczna w Diagnostyce (karta Uruchamianie). Obie wersje używają jednej blokady, więc
naraz działa tylko jedna: druga walczyłaby z pierwszą o port COM i vJoy.

## Testy

Nie potrzebują płytki ani sterowników.

```
bin\tests\core_tests.exe                  protokół, config.json, przetwarzanie, FFB, silnik
bin\Kierownica.exe --selftest katalog     wszystkie zakładki do PNG + zachowanie okna
```

Zgodność z wersją Python sprawdza test wzorcowy: kod Pythona liczy wyniki dla
losowych ramek i uszkodzonych plików `config.json`, a C++ musi dać to samo.

```
py -3 -B tests\make_golden.py golden.json
bin\tests\core_tests.exe golden.json
```

## Co jest inaczej niż w wersji Python

* Tekstu dziennika nie da się zaznaczać myszą - służy do tego przycisk
  **Kopiuj**.
* Rozwijane listy i menu nie wychodzą poza okno panelu.
* Nie ma szukania `ViGEmClient.dll` w pakiecie pip `vgamepad`; biblioteka jest
  wbudowana w program.

## Układ kodu

| Plik | Odpowiednik w Pythonie |
|---|---|
| `src/protocol`, `config`, `processing`, `ffb`, `link`, `engine`, `winsys` | moduły `wheelbridge` o tych samych nazwach |
| `src/outputs/vjoy`, `xinput`, `base.h` | `wheelbridge/outputs` |
| `src/ui/gfx` | `ui/art.py`, paleta z `ui/theme.py` |
| `src/ui/widgets` | `ui/widgets.py` |
| `src/ui/padview` | `ui/padview.py`, `ui/wheelview.py` |
| `src/ui/app` | okno, pasek u góry i odświeżanie z `ui/app.py` |
| `src/ui/page_home`, `page_setup`, `page_assign`, `page_diag` | zakładki z `ui/app.py`, `ui/g29tab.py` |
| `src/json` | moduł `json` Pythona |
| `src/embedded`, `src/kierownica.rc` | nowe: pliki wbudowane w `.exe`, ikony, VERSIONINFO |
| `src/ui/tray` | nowe: ikona w zasobniku (menu i logika w `ui/app`) |
| `src/updater` | nowe: aktualizacje z wydań GitHuba (WinHTTP, SHA-256 przez BCrypt) |
| `src/version.h`, `installer/`, `wydanie.bat`, `tools/make_icon.py` | nowe: wersja i wydanie |

Różnica, o której trzeba pamiętać przy zmianach: Python miał GIL, a tu wątki
naprawdę działają równolegle. Konfigurację zmienia wyłącznie wątek interfejsu,
a wątki łącza i FFB czytają niezmienną kopię (`WheelEngine::live()`) publikowaną
przez `publish_config()`.

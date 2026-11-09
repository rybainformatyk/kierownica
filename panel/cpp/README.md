# Panel kierownicy - wersja C++

Ta sama aplikacja co w `../python`, napisana w C++20 na czystym Win32 i
Direct2D. Wynik to jeden plik `Kierownica.exe` bez zależności: nie trzeba
Pythona, pyserial ani Pillow. Opis trybów, zakładek i protokołu jest w
[`../python/README.md`](../python/README.md) - dotyczy obu wersji.

## Budowanie

Potrzebne Visual Studio 2022 albo Build Tools z komponentem C++ (CMake i Ninja
są w pakiecie).

```
build.bat
```

Wynik trafia do `bin\`: `Kierownica.exe`, katalog `vigem\` z biblioteką pada
i `tests\core_tests.exe`.

## Uruchomienie

```
bin\Kierownica.exe
bin\Kierownica.exe --minimized            start zminimalizowany (autostart)
bin\Kierownica.exe --config plik.json     inny plik ustawień
```

Ustawienia są w `bin\config.json`, w tym samym formacie co w wersji Python -
plik można przenosić między wersjami. Obie wersje używają jednej blokady, więc
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
  **Kopiuj do schowka**.
* Rozwijane listy i menu nie wychodzą poza okno panelu.
* Nie ma szukania `ViGEmClient.dll` w pakiecie pip `vgamepad`; biblioteka leży
  w `bin\vigem`.

## Układ kodu

| Plik | Odpowiednik w Pythonie |
|---|---|
| `src/protocol`, `config`, `processing`, `ffb`, `link`, `engine`, `winsys` | moduły `wheelbridge` o tych samych nazwach |
| `src/outputs/vjoy`, `xinput`, `base.h` | `wheelbridge/outputs` |
| `src/ui/gfx` | `ui/art.py`, paleta z `ui/theme.py` |
| `src/ui/widgets` | `ui/widgets.py` |
| `src/ui/padview` | `ui/padview.py`, `ui/wheelview.py` |
| `src/ui/app` | `ui/app.py`, `ui/g29tab.py` |
| `src/json` | moduł `json` Pythona |

Różnica, o której trzeba pamiętać przy zmianach: Python miał GIL, a tu wątki
naprawdę działają równolegle. Konfigurację zmienia wyłącznie wątek interfejsu,
a wątki łącza i FFB czytają niezmienną kopię (`WheelEngine::live()`) publikowaną
przez `publish_config()`.

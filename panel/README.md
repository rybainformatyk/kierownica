# Panel kierownicy

Aplikacja na PC do kierownicy zbudowanej na Arduino Due. Płytka wysyła surowe
odczyty po porcie szeregowym, a panel zamienia je na urządzenie wirtualne,
które widzą gry.

## Tryby wyjścia

| Tryb | Co widzi gra | Do czego |
|---|---|---|
| **Kierownica G29 (Native USB)** — domyślny | prawdziwą kierownicę Logitech G29 (`046D:C24F`) | każda gra z obsługą kierownic, także Forza, która przyjmuje tylko kierownice ze swojej listy |
| **G29 + vJoy** | G29 do jazdy i obok vJoy z samymi przyciskami (osie vJoy stoją na środku) | gdy 25 przycisków G29 nie wystarcza — vJoy działa jak button box (zakładka Przyciski, do 32 przycisków + krzyżak) |
| **Kierownica (vJoy)** | urządzenie DirectInput: osobne osie pedałów, 32 przyciski, Force Feedback | symulatory — Assetto Corsa, iRacing, BeamNG, Dirt, F1 |
| **Pad Xbox 360 (ViGEm)** | zwykły kontroler XInput: dowolne źródło na dowolnym kierunku drążka lub spuście | gry czytające tylko XInput — NFS i większość gier zręcznościowych |
| **Oba naraz** | oba urządzenia jednocześnie | bez przełączania między grami |
| **Tylko podgląd** | nic | kalibracja i testy bez sterowników |

Tryb przełącza się na **Pulpicie** (karta *Wyjście do gier*) albo w
zakładkach **G29**, **Pad** i **Diagnostyka** — wszystkie pokazują to samo.

### Gry widzą tylko urządzenie z wybranego trybu

Żeby gra nie dostawała dwóch kierownic naraz (i nie myliła, która jest
która), panel ukrywa urządzenie, którego bieżący tryb nie używa:

| Tryb | G29 | vJoy |
|---|---|---|
| Kierownica G29 | widoczna | wyłączone |
| G29 + vJoy | widoczna | włączone |
| Kierownica (vJoy), Oba naraz | odłączona | włączone |
| Pad Xbox 360 | odłączona | wyłączone |
| Tylko podgląd | odłączona | bez zmian |

* **G29** odłącza sama płytka (firmware 2.2) — Windows widzi to jak
  wyjęcie kabla Native USB. Działa od razu, bez uprawnień.
* **vJoy** wyłącza panel przez `vJoyConfig enable off` (z instalacji vJoy).
  To wymaga administratora, więc Windows zapyta o zgodę — tylko przy
  zmianie trybu, bo stan zostaje w systemie także po restarcie. Panel
  przełącza sterownik w tle: okno działa dalej, a nagłówek i Pulpit
  pokazują, że trwa włączanie (urządzenie vJoy otwiera się samo, gdy
  sterownik ruszy). Po odmowie panel nie pyta ponownie aż do kliknięcia
  **Podłącz ponownie** w Diagnostyce.
* Pad ViGEm istnieje tylko wtedy, gdy panel go otworzy, więc nie trzeba go
  ukrywać.

Zachowanie wyłącza pole *Ukrywaj przed grami urządzenie, którego tryb nie
używa* w zakładce Diagnostyka — wtedy G29 i vJoy zostają widoczne jak
dawniej (vJoy jest tylko włączane, gdy tryb go potrzebuje).

### Kierownica G29

W tym trybie nie ma sterowników po stronie PC: port **Native USB** płytki
sam przedstawia się Windowsowi jako *G29 Driving Force Racing Wheel*, z
oryginalnym układem raportu (kierownica 16 bit, gaz/hamulec/sprzęgło
8 bit, 25 przycisków, krzyżak). Kalibrację i mapowanie dalej liczy panel —
składa gotowy raport G29 i odsyła go do płytki, a ta wypycha go przez USB.

1. Wgraj firmware 2.2: przycisk **Wgraj firmware** w Diagnostyce (panel sam
   zwolni port i połączy się ponownie) albo
   `../firmware/kierownica/wgraj_firmware.bat` przy zamkniętym panelu.
2. Podłącz **dwa kable**: Programming Port (łącze z panelem) i Native USB
   (kierownica dla gier).
3. W panelu wybierz tryb **Kierownica G29**. Karta *Kierownica G29* w
   zakładce Diagnostyka pokazuje, czy Windows widzi urządzenie i czy raporty
   docierają.

Przyciski G29 ustawiasz w zakładce **G29**: rysunek kierownicy (obręcz,
łopatki, przyciski, pokrętło, skrzynia biegów) i lista 29 gniazd. Kliknij
przycisk na rysunku albo wiersz na liście i wciśnij go na swojej kierownicy;
„Przypisz po kolei” przeprowadzi przez wszystkie po kolei. Te przypisania są
**własne** — nie zależą od zakładki Pad ani od listy vJoy w zakładce
Przyciski. Przycisk „Przepisz z pada” przenosi ustawienia z zakładki Pad na
odpowiedniki G29 (A → krzyżyk, B → kółko, LB/RB → łopatki …); stara
konfiguracja bez przypisań G29 jest przenoszona tak samo przy pierwszym
uruchomieniu. Gdy panel
zamilknie na 250 ms, G29 wraca do pozycji spoczynkowej — auto nie zostanie
z wciśniętym gazem.

Ograniczenie: Force Feedback **z gry** w trybie G29 nie działa — gra wysyła
go do sterownika Logitecha, którego tu nie ma. Lokalne efekty (sprężyna,
tłumienie, tarcie) działają. Komendy, które gra mimo to wyśle do G29, panel
liczy i pokazuje w karcie G29.

### Dlaczego dwa tryby

Gry zręcznościowe często w ogóle nie obsługują DirectInput — kierownica na vJoy
jest dla nich niewidzialna. Z kolei pad XInput ma tylko 8-bitowe spusty
(0–255) i mały zakres drążka, więc do symulatora się nie nadaje. Stąd dwa
osobne wyjścia zamiast jednego kompromisu.

W trybie pada ustaw **zakres kierownicy na pełne wychylenie drążka** na
270–450°. Pełne 900° oznaczałoby, że auto ledwo skręca.

## Instalacja

```
python -m pip install -r requirements.txt
```

Instaluje pyserial (łącze z płytką) i Pillow (wygładzone rysunki pada i
kierownicy).

Uruchomienie: `Kierownica.bat` albo `python kierownica.py`.

### Sterowniki

* **Tryb kierownicy** wymaga [vJoy](https://sourceforge.net/projects/vjoystick/).
  W `vJoyConf` włącz Device 1 z osiami X, Y, Z, Rx, Ry, Rz, 32 przyciskami,
  jednym POV i zaznaczonym **Enable Effects** (to jest Force Feedback).
* **Tryb pada** wymaga [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases).
  Biblioteka kliencka `ViGEmClient.dll` leży już w katalogu `vigem/`
  (MIT, z pakietu vgamepad — szczegóły w `vigem/README.txt`).

Zakładka **Diagnostyka** pokazuje, czego brakuje i co jest źle skonfigurowane.
Bez sterowników aplikacja nadal działa w trybie podglądu.

## Zakładki

* **Pulpit** — kąt obrotu, pionowe wskaźniki pedałów i osi AUX (wyłączony
  kanał pokazuje „wył.”), przyciski, co idzie na wyjście, moment FFB i wybór
  trybu wyjścia. Pod stanem wyjścia pojawia się podpowiedź, gdy coś
  wymaga uwagi (np. brak kabla Native USB albo starszy firmware na płytce).
* **Kierownica** — zakres lock-to-lock, impulsy enkodera (z autowykrywaniem),
  krzywa, miękki ogranicznik końca zakresu. Wykres krzywej skrętu pokazuje
  kąt kierownicy → oś w grze, a kropka — gdzie jest teraz kierownica.
* **Pedały** — kreator kalibracji, martwe strefy, krzywe, filtrowanie. Każdy
  pedał ma wykres krzywej: zacienione pasy to martwe strefy, kropka to
  bieżące wciśnięcie. Wykres liczy ta sama funkcja co silnik, więc pokazuje
  dokładnie to, co dostaje gra (bez wygładzania).
* **Przyciski** — mapowanie na przyciski vJoy, krzyżak, kolumna **NC** dla
  styków normalnie zwartych.
* **Pad** — rysowany kontroler i obok niego lista przypisań, po jednym
  wierszu na gniazdo. **Każdy kierunek drążka jest osobnym gniazdem**
  (w lewo, w prawo, w górę, w dół), tak jak w x360ce — dzięki temu w jeden
  drążek można wpiąć dwa różne źródła: skręt w poziomie, a gaz i hamulec
  w pionie. Mapować można z listy albo prosto z rysunku: przycisk na
  rysunku uczy się z kierownicy (kliknij, wciśnij przycisk), spust otwiera
  listę źródeł (kierownica w lewo / w prawo, gaz, hamulec, sprzęgło, AUX),
  a drążek najpierw pyta o kierunek. Prawy przycisk myszy czyści
  (na drążku - wszystkie cztery kierunki). Najechanie na wiersz podświetla
  odpowiednią część rysunku i odwrotnie. W wąskim oknie lista przechodzi
  pod rysunek.
  **Przypisz po kolei** prowadzi przez wszystkie 15 kontrolek krok po
  kroku — cel miga na rysunku, podpowiedź pokazuje, co gniazdo ma teraz
  i które przyciski trzymasz, a *Wstecz*, *Pomiń* i *Wyczyść i pomiń*
  pozwalają poprawić się bez zaczynania od nowa. Esc kończy.
* **Force Feedback** — wzmocnienia, efekty lokalne, ustawienia mostka H,
  interpretacja kierunku efektów z gry (zapisywana w ustawieniach).
* **Diagnostyka** — jakość łącza, stan vJoy/ViGEm, dziennik. **Wgraj
  firmware** rozłącza panel, otwiera okno `wgraj_firmware.bat` i po jego
  zamknięciu łączy się z płytką ponownie. **Kopiuj do schowka** kopiuje
  dziennik razem z wersją panelu i firmware — do zgłoszenia błędu.

Ustawienia lądują w `config.json` obok aplikacji — przy **Zapisz
ustawienia** (obok przycisku pojawia się potwierdzenie) i przy zamknięciu
okna. Dopóki zmiany nie są zapisane, obok przycisku świeci się
„● Niezapisane zmiany”.

### Skróty klawiszowe

| Skrót | Działanie |
|---|---|
| Ctrl+S | zapisz ustawienia |
| Ctrl+1 … Ctrl+8 | przejdź do zakładki (Pulpit … Diagnostyka) |
| Esc | przerwij przypisywanie przycisków |

## Firmware

W `../firmware/kierownica/kierownica.ino`, moduł G29 w
`../firmware/kierownica/g29_usb.h`. Wgrywanie: `wgraj_firmware.bat` (używa
arduino-cli z Arduino IDE 2, sam znajduje port) albo przycisk **Wgraj
firmware** w Diagnostyce — podaje skryptowi port, na którym panel był
połączony, więc trafia też w klony Due z innym układem USB. Gdy płytka ma
starszy firmware niż ten w repozytorium, Pulpit to podpowiada. Łącze z panelem domyślnie
na **Programming Port**, 250000 baud (ta wartość dzieli się bez reszty przez zegary SAM3X i ATmega16U2,
więc nie ma błędu zegara).

Piny: enkoder 2/3, potencjometry A0–A5, przycisk 0 na pinie 4, przyciski 1–31
na pinach 22–52, opcjonalny mostek H na 5 (EN), 6 (RPWM), 7 (LPWM).

Kalibracja i mapowanie **nie są** w firmware — zmiana ustawień działa od razu,
bez programowania płytki.

### Protokół

Ramki binarne z CRC8 i automatyczną resynchronizacją:

```
0xAA 0x55 | typ (1B) | dlugosc (1B) | dane | CRC8 (1B)
```

Płytka wysyła `STATE` (pozycja enkodera, 6 kanałów ADC 12-bit, maska 32
przycisków, flagi) z częstotliwością 50–500 Hz. PC odsyła `FFB`, `CENTER`,
`SET_ENDSTOP`, `SET_RATE`, konfigurację i — w trybie G29 — `G29` z gotowym
12-bajtowym raportem oraz `G29_USB` (podłącz / odłącz G29). Płytka odsyła `G29_OUT`, gdy gra wyśle coś do G29. Opis typów: `wheelbridge/protocol.py`
— musi być zgodny z sekcją protokołu w `.ino`.

## Bezpieczeństwo Force Feedback

FFB jest **wyłączony po starcie** i trzeba go włączyć świadomie. Firmware ma
własny watchdog: brak komendy z PC przez zadany czas zwalnia silnik. Są też
ograniczenie narastania momentu i limit PWM. Przy pierwszym uruchomieniu z
silnikiem zacznij od małego wzmocnienia.

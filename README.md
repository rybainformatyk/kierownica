# Kierownica DIY

Domowa kierownica do gier na **Arduino Due** z aplikacją na PC. Dzięki niej
gry widzą kierownicę jako **Logitech G29**, wirtualny joystick vJoy albo pad
Xbox 360. Kalibracja, mapowanie przycisków i Force Feedback są ustawiane
w aplikacji, więc zmiana ustawień nie wymaga ponownego wgrywania firmware.

## Struktura

```
firmware/
  kierownica/     firmware dla Arduino Due (kierownica.ino + g29_usb.h)
                  i skrypt wgraj_firmware.bat
  skrzynia/       prototyp skrzyni biegów H (ArduinoJoystickLibrary, płytki z ATmega32U4)
panel/            aplikacja na PC (Python + Tkinter) - szczegóły w panel/README.md
```

## Jak to działa

```
enkoder, pedały, przyciski ──► Arduino Due ──(Programming Port, 250000 baud)──► panel na PC
                                    ▲                                              │
                                    └────────── gotowy raport G29 ◄────────────────┘
                               Native USB = "G29 Driving Force Racing Wheel" ──► gra
```

Płytka wysyła surowe odczyty ramkami binarnymi z CRC8. Panel je kalibruje
i mapuje, a potem, zależnie od trybu:

* **G29 (domyślny)**: odsyła płytce gotowy raport, a jej port Native USB
  przedstawia się Windowsowi jako prawdziwy G29 (`046D:C24F`). Działa
  w każdej grze z obsługą kierownic, także w Forzy. Nie wymaga sterowników.
* **vJoy**: kierownica DirectInput z Force Feedback, do symulatorów.
* **Pad Xbox 360 (ViGEm)**: do gier, które obsługują tylko XInput.

## Szybki start

1. Zainstaluj [Arduino IDE 2](https://www.arduino.cc/en/software), a w nim
   *Arduino SAM Boards* (Due).
2. Podłącz Due przez **Programming Port** i uruchom
   `firmware/kierownica/wgraj_firmware.bat`.
3. Podłącz drugi kabel do **Native USB**. Windows wykryje *G29 Driving Force
   Racing Wheel*.
4. Zainstaluj [Pythona 3.9+](https://www.python.org/downloads/), potem:
   ```
   cd panel
   python -m pip install -r requirements.txt
   Kierownica.bat
   ```
5. W zakładce **G29** przypisz przyciski i kliknij **Zapisz ustawienia**.

Opis zakładek, trybów, sterowników (vJoy, ViGEmBus) i protokołu jest
w [panel/README.md](panel/README.md).

## Podłączenie (Arduino Due)

| Element | Piny |
|---|---|
| Enkoder kierownicy | 2, 3 |
| Pedały i osie dodatkowe (potencjometry) | A0–A5 |
| Przycisk 0 | 4 |
| Przyciski 1–31 | 22–52 |
| Mostek H do FFB (opcjonalnie) | 5 (EN), 6 (RPWM), 7 (LPWM) |

## Licencje zewnętrzne

`panel/vigem/ViGEmClient.dll` pochodzi z pakietu
[vgamepad](https://pypi.org/project/vgamepad/) (licencja MIT, treść
w `panel/vigem/LICENSE-vgamepad.txt`). Deskryptor raportu G29
w `g29_usb.h` jest oparty na projekcie
[lgff_wheel_adapter](https://github.com/sonik-br/lgff_wheel_adapter).

/* =============================================================================
 *  G29 - wspolne dla wszystkich plytek
 * =============================================================================
 *
 *  Port USB plytki przedstawia sie Windowsowi jako Logitech G29 Driving
 *  Force Racing Wheel: VID 046D, PID C24F i ten sam deskryptor raportu co
 *  prawdziwa kierownica w trybie PS3/PC. Gry, ktore przyjmuja tylko
 *  kierownice ze swojej listy (Forza), widza wtedy G29 - bez vJoy, bez
 *  trybu pada.
 *
 *  Raport sklada aplikacja na PC (kalibracja, krzywe, mapowanie przyciskow)
 *  i przysyla go gotowego komenda CMD_G29. Plytka tylko wypycha go przez
 *  USB, wiec cale mapowanie zostaje w aplikacji.
 *
 *  Uklad raportu - 12 B, bez numeru raportu, identyczny z G29:
 *    bajt 0      bity 0-3 krzyzak (0..7 co 45 st., 8 = srodek)
 *                bity 4-7 przyciski 1-4
 *    bajty 1-3   przyciski 5-25 (3 najstarsze bity bajtu 3 sa puste)
 *    bajty 4-5   kierownica, 16 bit LE, 0..65535, srodek 32768
 *    bajt 6      gaz       (os Z)   255 = puszczony
 *    bajt 7      hamulec   (os Rz)  255 = puszczony
 *    bajt 8      sprzeglo  (os Y)   255 = puszczony
 *    bajty 9-11  pole producenta (drazek skrzyni, flagi)
 *
 *  Deskryptor raportu pochodzi z projektu lgff_wheel_adapter
 *  (github.com/sonik-br/lgff_wheel_adapter, usb_descriptors.h, licencja
 *  GPL-3.0). Zgadza sie z parserem G29 w SDL3 (SDL_hidapi_lg4ff.c: raport
 *  12 B, kierownica w bajtach 4-5, pedaly w 6-8) i z zapisem sterownika
 *  Logitecha (17-bajtowy zapis = numer raportu + 16 B wyjscia).
 *
 *  Implementacje: g29_usb.h (Due, Leonardo - PluggableUSB),
 *  g29_tinyusb.h (ESP32-S3 - TinyUSB).
 * =============================================================================
 */

#pragma once

#include <Arduino.h>

#define G29_VID           0x046D
#define G29_PID           0xC24F
#define G29_BCD_DEVICE    0x8900      // rewizja rozpoznawana jako G29 (SDL, Linux)
#define G29_REPORT_SIZE   12
#define G29_OUT_MAX       16          // raport wyjsciowy gry (komendy FFB/zakresu)

static const char G29_MANUFACTURER[] = "Logitech";
static const char G29_PRODUCT[]      = "G29 Driving Force Racing Wheel";

// W AVR deskryptor siedzi we flashu - 2,5 KB RAM Leonardo to malo.
static const uint8_t G29_REPORT_DESC[] PROGMEM = {
  0x05, 0x01, 0x09, 0x04, 0xA1, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
  0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75, 0x04, 0x95, 0x01, 0x81,
  0x42, 0x05, 0x09, 0x19, 0x01, 0x29, 0x19, 0x65, 0x00, 0x25, 0x01, 0x45,
  0x01, 0x75, 0x01, 0x95, 0x19, 0x81, 0x02, 0x75, 0x01, 0x95, 0x03, 0x81,
  0x03, 0x05, 0x01, 0x09, 0x30, 0x27, 0xFF, 0xFF, 0x00, 0x00, 0x47, 0xFF,
  0xFF, 0x00, 0x00, 0x75, 0x10, 0x95, 0x01, 0x81, 0x02, 0x09, 0x32, 0x09,
  0x35, 0x09, 0x31, 0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, 0x75, 0x08, 0x95,
  0x03, 0x81, 0x02, 0x06, 0x00, 0xFF, 0x09, 0x01, 0x95, 0x02, 0x81, 0x02,
  0x06, 0x00, 0xFF, 0x09, 0x01, 0x95, 0x01, 0x81, 0x02, 0x06, 0x00, 0xFF,
  0x09, 0x02, 0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00, 0x95, 0x10, 0x75, 0x08,
  0x91, 0x02, 0xC0,
};

// Stan spoczynkowy: krzyzak na srodku, nic nie wcisniete, kierownica na
// srodku, pedaly puszczone. Wysylany po starcie i gdy aplikacja zamilknie.
static const uint8_t G29_NEUTRAL[G29_REPORT_SIZE] = {
  0x08, 0x00, 0x00, 0x00,
  0x00, 0x80,
  0xFF, 0xFF, 0xFF,
  0x80, 0x80, 0x00,
};

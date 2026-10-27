/* =============================================================================
 *  G29 na porcie NATIVE USB
 * =============================================================================
 *
 *  Port Native USB (gniazdo micro-USB blizej srodka plytki) przedstawia sie
 *  Windowsowi jako Logitech G29 Driving Force Racing Wheel: VID 046D,
 *  PID C24F i ten sam deskryptor raportu co prawdziwa kierownica w trybie
 *  PS3/PC. Gry, ktore przyjmuja tylko kierownice ze swojej listy (Forza),
 *  widza wtedy G29 - bez vJoy, bez trybu pada.
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
 *  (github.com/sonik-br/lgff_wheel_adapter, usb_descriptors.h). Zgadza sie
 *  z parserem G29 w SDL3 (SDL_hidapi_lg4ff.c: raport 12 B, kierownica w
 *  bajtach 4-5, pedaly w 6-8) i z zapisem sterownika Logitecha (17-bajtowy
 *  zapis = numer raportu + 16 B wyjscia).
 *
 *  Kazda funkcja wolana z petli jest NIEBLOKUJACA. USBD_Send() z rdzenia Due
 *  czeka w nieskonczonosc, az host odbierze poprzedni pakiet - przy uspionym
 *  komputerze zawiesilaby petle razem z watchdogiem silnika FFB. Dlatego
 *  przed wyslaniem sami sprawdzamy, czy bufor endpointu jest wolny.
 * =============================================================================
 */

#pragma once

#include <Arduino.h>
#include "USB/PluggableUSB.h"

#define G29_VID           0x046D
#define G29_PID           0xC24F
#define G29_BCD_DEVICE    0x8900      // rewizja rozpoznawana jako G29 (SDL, Linux)
#define G29_REPORT_SIZE   12
#define G29_OUT_MAX       16          // raport wyjsciowy gry (komendy FFB/zakresu)

// zadania klasy HID - wlasne nazwy, zeby nie wciagac biblioteki HID
#define G29_HID_GET_REPORT     0x01
#define G29_HID_GET_IDLE       0x02
#define G29_HID_GET_PROTOCOL   0x03
#define G29_HID_SET_REPORT     0x09
#define G29_HID_SET_IDLE       0x0A
#define G29_HID_SET_PROTOCOL   0x0B
#define G29_DESC_HID           0x21
#define G29_DESC_REPORT        0x22

static const uint8_t G29_REPORT_DESC[] = {
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

static const char G29_MANUFACTURER[] = "Logitech";
static const char G29_PRODUCT[]      = "G29 Driving Force Racing Wheel";

class G29Usb : public PluggableUSBModule {
public:
  G29Usb() : PluggableUSBModule(1, 1, epType) {
    epType[0] = EP_TYPE_INTERRUPT_IN;
    memcpy(report, G29_NEUTRAL, G29_REPORT_SIZE);
    PluggableUSB().plug(this);
  }

  // Raport od aplikacji. False przy zlej dlugosci.
  bool setReport(const uint8_t *p, uint8_t len) {
    if (len != G29_REPORT_SIZE) return false;
    noInterrupts();                       // GET_REPORT czyta go w przerwaniu
    if (memcmp(report, p, G29_REPORT_SIZE) != 0) {
      memcpy(report, p, G29_REPORT_SIZE);
      dirty = true;
    }
    interrupts();
    return true;
  }

  void setNeutral() { setReport(G29_NEUTRAL, G29_REPORT_SIZE); }

  // Czy Windows wyliczyl port Native USB (kabel podlaczony, sterownik
  // zaladowany). Trafia do flag ramki STATE.
  bool hostConfigured() const { return USBDevice.configured(); }

  // Wolane w kazdym obiegu petli. Nigdy nie czeka na hosta.
  void poll() {
    if (!USBDevice.configured()) {
      wasConfigured = false;
      return;
    }
    if (!wasConfigured) {                 // swiezo wyliczony - wyslij stan
      wasConfigured = true;
      dirty = true;
    }
    if (!dirty) return;

    const uint32_t ep = pluggedEndpoint;
    // TXINI = jest wolny bank endpointu. Bez tego USBD_Send by czekal.
    if (!(UOTGHS->UOTGHS_DEVEPTISR[ep] & UOTGHS_DEVEPTISR_TXINI)) return;

    uint8_t copy[G29_REPORT_SIZE];
    noInterrupts();
    memcpy(copy, report, G29_REPORT_SIZE);
    dirty = false;
    interrupts();
    USBD_Send(ep, copy, G29_REPORT_SIZE);
  }

  // Raport wyjsciowy od gry (sterownik Logitecha wysyla tak komendy FFB
  // i zakresu obrotu). Zwraca dlugosc albo 0, gdy nic nie przyszlo.
  uint8_t takeOutput(uint8_t *dst) {
    if (!outPending) return 0;
    noInterrupts();
    uint8_t n = outLen;
    memcpy(dst, outBuf, n);
    outPending = false;
    interrupts();
    return n;
  }

protected:
  int getInterface(uint8_t *interfaceCount) {
    *interfaceCount += 1;
    const uint16_t rlen = sizeof(G29_REPORT_DESC);
    uint8_t d[9 + 9 + 7] = {
      // interfejs: 1 endpoint, klasa HID, bez podklasy boot
      9, 0x04, pluggedInterface, 0x00, 1, 0x03, 0x00, 0x00, 0x00,
      // deskryptor HID 1.11, jeden deskryptor raportu
      9, G29_DESC_HID, 0x11, 0x01, 0x00, 1, G29_DESC_REPORT,
      (uint8_t)(rlen & 0xFF), (uint8_t)(rlen >> 8),
      // endpoint IN, przerwaniowy, 64 B, co 1 ramke
      7, 0x05, (uint8_t)(0x80 | pluggedEndpoint), 0x03, 0x40, 0x00, 0x01,
    };
    return USBD_SendControl(0, d, sizeof(d));
  }

  int getDescriptor(USBSetup &setup) {
    // Deskryptory urzadzenia i napisy - to one mowia Windowsowi "to jest G29".
    // Rdzen pyta moduly PRZED wyslaniem wlasnych, wiec wystarczy odpowiedziec.
    if (setup.bmRequestType == REQUEST_DEVICETOHOST) {
      if (setup.wValueH == USB_DEVICE_DESCRIPTOR_TYPE) {
        const uint8_t d[18] = {
          18, 0x01, 0x00, 0x02,           // USB 2.0
          0xEF, 0x02, 0x01,               // urzadzenie zlozone z IAD (CDC + HID)
          64,
          (uint8_t)(G29_VID & 0xFF), (uint8_t)(G29_VID >> 8),
          (uint8_t)(G29_PID & 0xFF), (uint8_t)(G29_PID >> 8),
          (uint8_t)(G29_BCD_DEVICE & 0xFF), (uint8_t)(G29_BCD_DEVICE >> 8),
          IMANUFACTURER, IPRODUCT, ISERIAL,
          1,
        };
        return sendClipped(d, sizeof(d), setup.wLength);
      }
      if (setup.wValueH == USB_STRING_DESCRIPTOR_TYPE) {
        if (setup.wValueL == IMANUFACTURER) return sendString(G29_MANUFACTURER, setup.wLength);
        if (setup.wValueL == IPRODUCT)      return sendString(G29_PRODUCT, setup.wLength);
      }
      return 0;
    }

    // Deskryptory HID naszego interfejsu
    if (setup.bmRequestType == REQUEST_DEVICETOHOST_STANDARD_INTERFACE &&
        setup.wIndex == pluggedInterface) {
      if (setup.wValueH == G29_DESC_REPORT) {
        return sendClipped(G29_REPORT_DESC, sizeof(G29_REPORT_DESC), setup.wLength);
      }
      if (setup.wValueH == G29_DESC_HID) {
        const uint16_t rlen = sizeof(G29_REPORT_DESC);
        const uint8_t d[9] = { 9, G29_DESC_HID, 0x11, 0x01, 0x00, 1, G29_DESC_REPORT,
                               (uint8_t)(rlen & 0xFF), (uint8_t)(rlen >> 8) };
        return sendClipped(d, sizeof(d), setup.wLength);
      }
    }
    return 0;
  }

  bool setup(USBSetup &setup) {
    if (setup.wIndex != pluggedInterface) return false;
    const uint8_t r = setup.bRequest;
    const uint8_t t = setup.bmRequestType;

    if (t == REQUEST_DEVICETOHOST_CLASS_INTERFACE) {
      if (r == G29_HID_GET_REPORT) {
        uint8_t copy[G29_REPORT_SIZE];
        memcpy(copy, report, G29_REPORT_SIZE);
        sendClipped(copy, G29_REPORT_SIZE, setup.wLength);
        return true;
      }
      if (r == G29_HID_GET_IDLE)     { USBD_SendControl(0, &idle, 1);     return true; }
      if (r == G29_HID_GET_PROTOCOL) { USBD_SendControl(0, &protocol, 1); return true; }
    }

    if (t == REQUEST_HOSTTODEVICE_CLASS_INTERFACE) {
      if (r == G29_HID_SET_IDLE)     { idle = setup.wValueH;     return true; }
      if (r == G29_HID_SET_PROTOCOL) { protocol = setup.wValueL; return true; }
      if (r == G29_HID_SET_REPORT) {
        // Faza danych MUSI zostac odebrana w calosci, inaczej host zgubi
        // transfer. USBD_RecvControl dziala do 64 B - raport G29 ma 16.
        const uint16_t n = setup.wLength;
        if (n > 64) return false;
        if (n == 0) return true;
        uint8_t buf[64];
        USBD_RecvControl(buf, n);
        const uint8_t keep = (n > G29_OUT_MAX) ? G29_OUT_MAX : (uint8_t)n;
        memcpy(outBuf, buf, keep);
        outLen = keep;
        outPending = true;
        return true;
      }
    }
    return false;
  }

  uint8_t getShortName(char *name) {
    // numer seryjny - staly, zeby Windows nie tworzyl nowego urzadzenia
    // przy kazdym podlaczeniu
    name[0] = 'G'; name[1] = '2'; name[2] = '9';
    return 3;
  }

private:
  static int sendClipped(const uint8_t *d, uint16_t len, uint16_t wLength) {
    // USBD_SendControl nie przycina do dlugosci, o ktora pyta host
    if (len > wLength) len = wLength;
    if (len == 0) return 0;
    USBD_SendControl(0, d, len);
    return len;
  }

  static int sendString(const char *s, uint16_t wLength) {
    uint8_t buf[2 + 2 * 40];
    uint8_t n = 0;
    while (s[n] && n < 40) n++;
    buf[0] = (uint8_t)(2 + 2 * n);
    buf[1] = USB_STRING_DESCRIPTOR_TYPE;
    for (uint8_t i = 0; i < n; i++) {     // ASCII -> UTF-16LE
      buf[2 + 2 * i] = (uint8_t)s[i];
      buf[3 + 2 * i] = 0;
    }
    return sendClipped(buf, buf[0], wLength);
  }

  uint32_t epType[1];
  uint8_t  report[G29_REPORT_SIZE];
  volatile bool dirty = true;
  bool     wasConfigured = false;
  uint8_t  idle = 0;
  uint8_t  protocol = 1;

  volatile bool outPending = false;
  uint8_t  outLen = 0;
  uint8_t  outBuf[G29_OUT_MAX];
};

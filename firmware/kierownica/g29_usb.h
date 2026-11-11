/* =============================================================================
 *  G29 przez PluggableUSB - Arduino Due (Native USB) i Leonardo / Micro
 * =============================================================================
 *
 *  Opis raportu: g29_raport.h. Tu jest tylko obsluga USB.
 *
 *  Due: port Native USB (gniazdo blizej srodka plytki) jest sama kierownica,
 *  a lacze z aplikacja idzie Programming Portem. setAttached(false) odlacza
 *  caly port Native USB od komputera - Windows widzi to jak wyjecie kabla,
 *  wiec gry przestaja widziec G29. Aplikacja robi tak w trybach bez G29.
 *
 *  Leonardo / Micro: jedno gniazdo USB jest urzadzeniem zlozonym - port COM
 *  (lacze z aplikacja) + G29. Odlaczyc G29 sie nie da, bo zniknalby tez
 *  port COM.
 *
 *  Kazda funkcja wolana z petli jest NIEBLOKUJACA. USBD_Send() (Due)
 *  i USB_Send() (AVR) czekaja, az host odbierze poprzedni pakiet - przy
 *  uspionym komputerze zawiesilyby petle razem z watchdogiem silnika FFB.
 *  Dlatego przed wyslaniem sami sprawdzamy, czy bufor endpointu jest wolny.
 * =============================================================================
 */

#pragma once

#include <Arduino.h>
#include "g29_raport.h"

#if defined(ARDUINO_ARCH_SAM)
  #include "USB/PluggableUSB.h"
  typedef uint32_t G29EpType;
  #define G29_SEND_CONTROL  USBD_SendControl
  #define G29_RECV_CONTROL  USBD_RecvControl
  #define G29_PGM_FLAG      0
  #define G29_CAN_DETACH    1
#elif defined(ARDUINO_ARCH_AVR) && defined(USBCON)
  #include "PluggableUSB.h"
  typedef uint8_t G29EpType;
  #define G29_SEND_CONTROL  USB_SendControl
  #define G29_RECV_CONTROL  USB_RecvControl
  #define G29_PGM_FLAG      TRANSFER_PGM
  #define G29_CAN_DETACH    0
#else
  #error "g29_usb.h wymaga Arduino Due albo plytki z ATmega32U4"
#endif

// zadania klasy HID - wlasne nazwy, zeby nie wciagac biblioteki HID
#define G29_HID_GET_REPORT     0x01
#define G29_HID_GET_IDLE       0x02
#define G29_HID_GET_PROTOCOL   0x03
#define G29_HID_SET_REPORT     0x09
#define G29_HID_SET_IDLE       0x0A
#define G29_HID_SET_PROTOCOL   0x0B
#define G29_DESC_HID           0x21
#define G29_DESC_REPORT        0x22

class G29Usb : public PluggableUSBModule {
public:
  G29Usb() : PluggableUSBModule(1, 1, epType) {
    epType[0] = EP_TYPE_INTERRUPT_IN;
    memcpy(report, G29_NEUTRAL, G29_REPORT_SIZE);
    PluggableUSB().plug(this);
  }

  void begin() {}

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

  // Czy Windows wyliczyl G29 (kabel podlaczony, sterownik zaladowany).
  // Trafia do flag ramki STATE.
  bool hostConfigured() { return usbOn && USBDevice.configured(); }

  // Podlacza / odlacza port Native USB (tylko Due). Po odlaczeniu rdzen nie
  // dostaje juz resetu od hosta, wiec configured() zostaje na starej
  // wartosci - stad wlasna flaga usbOn.
  void setAttached(bool on) {
#if G29_CAN_DETACH
    if (on == usbOn) return;
    usbOn = on;
    wasConfigured = false;
    if (on) USBDevice.attach();
    else    USBDevice.detach();
#else
    (void)on;
#endif
  }

  bool attached() const { return usbOn; }

  // Wolane w kazdym obiegu petli. Nigdy nie czeka na hosta.
  void poll() {
    if (!usbOn || !USBDevice.configured()) {
      wasConfigured = false;
      return;
    }
    if (!wasConfigured) {                 // swiezo wyliczony - wyslij stan
      wasConfigured = true;
      dirty = true;
    }
    if (!dirty || !endpointFree()) return;

    uint8_t copy[G29_REPORT_SIZE];
    noInterrupts();
    memcpy(copy, report, G29_REPORT_SIZE);
    dirty = false;
    interrupts();
#if defined(ARDUINO_ARCH_SAM)
    USBD_Send(pluggedEndpoint, copy, G29_REPORT_SIZE);
#else
    USB_Send(pluggedEndpoint | TRANSFER_RELEASE, copy, G29_REPORT_SIZE);
#endif
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
    return G29_SEND_CONTROL(0, d, sizeof(d));
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
        return sendClipped(d, sizeof(d), setup.wLength, 0);
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
        return sendClipped(G29_REPORT_DESC, sizeof(G29_REPORT_DESC), setup.wLength, G29_PGM_FLAG);
      }
      if (setup.wValueH == G29_DESC_HID) {
        const uint16_t rlen = sizeof(G29_REPORT_DESC);
        const uint8_t d[9] = { 9, G29_DESC_HID, 0x11, 0x01, 0x00, 1, G29_DESC_REPORT,
                               (uint8_t)(rlen & 0xFF), (uint8_t)(rlen >> 8) };
        return sendClipped(d, sizeof(d), setup.wLength, 0);
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
        sendClipped(copy, G29_REPORT_SIZE, setup.wLength, 0);
        return true;
      }
      if (r == G29_HID_GET_IDLE)     { G29_SEND_CONTROL(0, &idle, 1);     return true; }
      if (r == G29_HID_GET_PROTOCOL) { G29_SEND_CONTROL(0, &protocol, 1); return true; }
    }

    if (t == REQUEST_HOSTTODEVICE_CLASS_INTERFACE) {
      if (r == G29_HID_SET_IDLE)     { idle = setup.wValueH;     return true; }
      if (r == G29_HID_SET_PROTOCOL) { protocol = setup.wValueL; return true; }
      if (r == G29_HID_SET_REPORT) {
        // Faza danych MUSI zostac odebrana w calosci, inaczej host zgubi
        // transfer. RecvControl dziala do 64 B - raport G29 ma 16.
        const uint16_t n = setup.wLength;
        if (n > 64) return false;
        if (n == 0) return true;
        uint8_t buf[64];
        G29_RECV_CONTROL(buf, n);
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
  // Czy endpoint przyjmie raport bez czekania.
  bool endpointFree() {
#if defined(ARDUINO_ARCH_SAM)
    // TXINI = jest wolny bank endpointu
    return UOTGHS->UOTGHS_DEVEPTISR[pluggedEndpoint] & UOTGHS_DEVEPTISR_TXINI;
#else
    return USB_SendSpace(pluggedEndpoint) >= G29_REPORT_SIZE;
#endif
  }

  static int sendClipped(const uint8_t *d, uint16_t len, uint16_t wLength, uint8_t flags) {
    // SendControl nie przycina do dlugosci, o ktora pyta host
    if (len > wLength) len = wLength;
    if (len == 0) return 0;
    G29_SEND_CONTROL(flags, d, len);
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
    return sendClipped(buf, buf[0], wLength, 0);
  }

  G29EpType epType[1];
  uint8_t  report[G29_REPORT_SIZE];
  volatile bool dirty = true;
  bool     wasConfigured = false;
  bool     usbOn = true;              // port podlaczony do komputera
  uint8_t  idle = 0;
  uint8_t  protocol = 1;

  volatile bool outPending = false;
  uint8_t  outLen = 0;
  uint8_t  outBuf[G29_OUT_MAX];
};

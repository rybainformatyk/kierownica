/* =============================================================================
 *  KIEROWNICA  -  firmware dla Arduino Due, Mega, Uno, Nano, Leonardo, Micro,
 *                 ESP32 i ESP32-S3
 *  Wersja 3.0  -  lacze szeregowe z aplikacja + kierownica G29 na USB
 * =============================================================================
 *
 *  NOWE W 3.0 - WIELE PLYTEK
 *    Ten sam szkic kompiluje sie na kilku plytkach. Piny i mozliwosci kazdej
 *    plytki sa w piny.h. Aplikacja rozpoznaje plytke po nazwie w HELLO
 *    (KIEROWNICA-DUE, -MEGA, -UNO, -LEONARDO, -ESP32, -ESP32S3), a o tym,
 *    co plytka umie, mowia bity CAP_*. Odczyty ADC zawsze trafiaja do
 *    aplikacji jako 0..4095 (10 bitow jest mnozone x4).
 *
 *  2.2 - G29 ZNIKA, GDY NIE JEST UZYWANA (tylko Due)
 *    Komenda CMD_G29_USB odlacza port Native USB od komputera (i podlacza go
 *    z powrotem). Aplikacja odlacza G29 w trybach bez G29 (vJoy, pad), zeby
 *    gry nie widzialy dwoch kierownic naraz.
 *
 *  2.1 - KIEROWNICA G29
 *    Port USB przedstawia sie Windowsowi jako Logitech G29 (VID 046D,
 *    PID C24F, oryginalny uklad raportu) - patrz g29_raport.h. Kalibracje
 *    i mapowanie robi aplikacja: liczy gotowy raport G29 i przysyla go
 *    komenda CMD_G29, a plytka tylko wypycha go przez USB. Gdy aplikacja
 *    zamilknie na 250 ms, G29 wraca do pozycji spoczynkowej - auto nie
 *    zostanie z wcisnietym gazem.
 *      Due:             Programming Port = lacze, Native USB = G29 (2 kable)
 *      Leonardo, Micro: jedno gniazdo = lacze + G29
 *      ESP32-S3:        gniazdo USB (OTG) = lacze + G29
 *      Mega, Uno, ESP32: bez G29 - tryby vJoy i pad Xbox
 *
 *  Plytka NIE udaje joysticka sama z siebie. Wysyla surowe odczyty ramkami
 *  binarnymi do aplikacji "panel" na PC, a ta tworzy urzadzenie, ktore gry
 *  widza jako kierownice (G29, vJoy albo pad Xbox 360).
 *
 *  Najwazniejsze zasady (z wersji 1):
 *    * nigdy nie blokujemy petli na write() - ramka bez miejsca jest pomijana,
 *    * nieblokujacy harmonogram na micros(), bez delay(),
 *    * dekoder kwadraturowy x4 na przerwaniach z wykrywaniem bledow,
 *    * filtr IIR na osiach, debounce wertykalnym licznikiem na 32 przyciskach,
 *    * ramki z CRC8 i automatyczna resynchronizacja,
 *    * kanal Force Feedback z watchdogiem (wylaczony po starcie).
 *
 *  POLACZENIE
 *    250000 baud - ta wartosc dzieli sie calkowicie przez zegary SAM3X
 *    (84 MHz / 16 / 21), ATmega16U2 i ATmega328P (16 MHz / 8 / 8), wiec nie
 *    ma bledu zegara. Przy natywnym USB predkosc nie ma znaczenia.
 *
 *  PINY - patrz piny.h
 * =============================================================================
 */

#include "piny.h"

// ---------------------------------------------------------------- konfiguracja

#define WHEEL_BAUD           250000 // ignorowane przy natywnym USB
#define G29_TIMEOUT_MS       250    // brak raportu z aplikacji -> pozycja spoczynkowa

#define TICK_HZ              1000   // czestotliwosc filtrow i debounce
#define DEFAULT_REPORT_HZ    333    // czestotliwosc ramek stanu
#define MIN_REPORT_HZ        50
#define MAX_REPORT_HZ        500

// --------------------------------------------------------------------- protokol

#define PROTO_VERSION  1
#define FW_MAJOR       3
#define FW_MINOR       0

#define SYNC1 0xAA
#define SYNC2 0x55

// plytka -> PC
#define MSG_HELLO  0x01
#define MSG_STATE  0x02
#define MSG_LOG    0x03
#define MSG_PONG   0x04
#define MSG_ACK    0x05
#define MSG_G29_OUT 0x06     // raport wyjsciowy, ktory gra wyslala do G29

// PC -> plytka
#define CMD_PING        0x10
#define CMD_IDENTIFY    0x11
#define CMD_CENTER      0x12
#define CMD_SET_ENDSTOP 0x13
#define CMD_SET_RATE    0x14
#define CMD_FFB         0x15
#define CMD_FFB_CONFIG  0x16
#define CMD_RESET       0x17
#define CMD_G29         0x18     // gotowy 12-bajtowy raport G29 z aplikacji
#define CMD_G29_USB     0x19     // 1 = podlacz G29 do komputera, 0 = odlacz

#define CAP_FFB        0x01
#define CAP_NATIVE_USB 0x02     // lacze z aplikacja idzie natywnym USB plytki
#define CAP_G29        0x04
#define CAP_G29_HIDE   0x08     // umie odlaczyc G29 komenda CMD_G29_USB

#define NUM_AXES    6
#define MAX_BUTTONS 32

#define MAX_PAYLOAD 64

// --------------------------------------------------------------------- port

#if WPORT_IS_TINYUSB
  #include "USB.h"
  #include "USBCDC.h"
  static USBCDC wheelCdc(0);
#endif

// ----------------------------------------------------------------------- G29

#if G29_PLUGGABLE
  #include "g29_usb.h"
#elif G29_TINYUSB
  #include "g29_tinyusb.h"
#endif

#if HAS_G29
  // Obiekt globalny: konstruktor rejestruje interfejs USB, zanim rdzen
  // podlaczy port do hosta.
  static G29Usb g29Usb;
  static uint32_t g29LastCmdMs = 0;
  static bool     g29Fed       = false;   // aplikacja przysyla raporty
#endif

// ---------------------------------------------------------------------- CRC8

// CRC-8/ATM, polinom 0x07, init 0x00 - tablica we flashu
static const uint8_t CRC8_TABLE[256] PROGMEM = {
  0x00,0x07,0x0E,0x09,0x1C,0x1B,0x12,0x15,0x38,0x3F,0x36,0x31,0x24,0x23,0x2A,0x2D,
  0x70,0x77,0x7E,0x79,0x6C,0x6B,0x62,0x65,0x48,0x4F,0x46,0x41,0x54,0x53,0x5A,0x5D,
  0xE0,0xE7,0xEE,0xE9,0xFC,0xFB,0xF2,0xF5,0xD8,0xDF,0xD6,0xD1,0xC4,0xC3,0xCA,0xCD,
  0x90,0x97,0x9E,0x99,0x8C,0x8B,0x82,0x85,0xA8,0xAF,0xA6,0xA1,0xB4,0xB3,0xBA,0xBD,
  0xC7,0xC0,0xC9,0xCE,0xDB,0xDC,0xD5,0xD2,0xFF,0xF8,0xF1,0xF6,0xE3,0xE4,0xED,0xEA,
  0xB7,0xB0,0xB9,0xBE,0xAB,0xAC,0xA5,0xA2,0x8F,0x88,0x81,0x86,0x93,0x94,0x9D,0x9A,
  0x27,0x20,0x29,0x2E,0x3B,0x3C,0x35,0x32,0x1F,0x18,0x11,0x16,0x03,0x04,0x0D,0x0A,
  0x57,0x50,0x59,0x5E,0x4B,0x4C,0x45,0x42,0x6F,0x68,0x61,0x66,0x73,0x74,0x7D,0x7A,
  0x89,0x8E,0x87,0x80,0x95,0x92,0x9B,0x9C,0xB1,0xB6,0xBF,0xB8,0xAD,0xAA,0xA3,0xA4,
  0xF9,0xFE,0xF7,0xF0,0xE5,0xE2,0xEB,0xEC,0xC1,0xC6,0xCF,0xC8,0xDD,0xDA,0xD3,0xD4,
  0x69,0x6E,0x67,0x60,0x75,0x72,0x7B,0x7C,0x51,0x56,0x5F,0x58,0x4D,0x4A,0x43,0x44,
  0x19,0x1E,0x17,0x10,0x05,0x02,0x0B,0x0C,0x21,0x26,0x2F,0x28,0x3D,0x3A,0x33,0x34,
  0x4E,0x49,0x40,0x47,0x52,0x55,0x5C,0x5B,0x76,0x71,0x78,0x7F,0x6A,0x6D,0x64,0x63,
  0x3E,0x39,0x30,0x37,0x22,0x25,0x2C,0x2B,0x06,0x01,0x08,0x0F,0x1A,0x1D,0x14,0x13,
  0xAE,0xA9,0xA0,0xA7,0xB2,0xB5,0xBC,0xBB,0x96,0x91,0x98,0x9F,0x8A,0x8D,0x84,0x83,
  0xDE,0xD9,0xD0,0xD7,0xC2,0xC5,0xCC,0xCB,0xE6,0xE1,0xE8,0xEF,0xFA,0xFD,0xF4,0xF3
};

static inline uint8_t crcStep(uint8_t c, uint8_t b) { return pgm_read_byte(&CRC8_TABLE[c ^ b]); }

static inline uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t c = 0x00;
  while (len--) c = crcStep(c, *data++);
  return c;
}

// ------------------------------------------------------------------- enkoder

// W ESP32 kod przerwania i jego dane musza lezec w RAM.
#if defined(ARDUINO_ARCH_ESP32)
  #define ISR_CODE IRAM_ATTR
  #define ISR_DATA DRAM_ATTR
#else
  #define ISR_CODE
  #define ISR_DATA
#endif

// Dekoder kwadraturowy x4. Indeks = (poprzedni_stan << 2) | nowy_stan,
// gdzie stan = (A << 1) | B. Zera na przekatnej, 0 przy przeskoku o 2 (blad).
static const int8_t ISR_DATA QUAD_TABLE[16] = {
   0, +1, -1,  0,
  -1,  0,  0, +1,
  +1,  0,  0, -1,
   0, -1, +1,  0
};

volatile int32_t  encCount    = 0;
volatile uint32_t encErrors   = 0;
volatile uint8_t  encState    = 0;

static int32_t   encOffset = 0;   // pozycja zera (ustawiana przez CMD_CENTER)

// Szybki odczyt pinow enkodera prosto z rejestrow - digitalRead() w
// przerwaniu bylby za wolny przy szybkim kreceniu kierownica.
#if defined(ARDUINO_ARCH_SAM)
  static Pio      *encAPort, *encBPort;
  static uint32_t  encAMask,  encBMask;
  static void encPinsInit() {
    encAPort = g_APinDescription[ENC_A_PIN].pPort;
    encAMask = g_APinDescription[ENC_A_PIN].ulPin;
    encBPort = g_APinDescription[ENC_B_PIN].pPort;
    encBMask = g_APinDescription[ENC_B_PIN].ulPin;
  }
  static inline bool encA() { return encAPort->PIO_PDSR & encAMask; }
  static inline bool encB() { return encBPort->PIO_PDSR & encBMask; }
#elif defined(ARDUINO_ARCH_AVR)
  static volatile uint8_t *encAPort, *encBPort;
  static uint8_t encAMask, encBMask;
  static void encPinsInit() {
    encAPort = portInputRegister(digitalPinToPort(ENC_A_PIN));
    encAMask = digitalPinToBitMask(ENC_A_PIN);
    encBPort = portInputRegister(digitalPinToPort(ENC_B_PIN));
    encBMask = digitalPinToBitMask(ENC_B_PIN);
  }
  static inline bool encA() { return *encAPort & encAMask; }
  static inline bool encB() { return *encBPort & encBMask; }
#elif defined(ARDUINO_ARCH_ESP32)
  #include "soc/gpio_reg.h"
  static_assert(ENC_A_PIN < 32 && ENC_B_PIN < 32, "enkoder na GPIO 0..31");
  static void encPinsInit() {}
  static inline bool ISR_CODE encA() { return REG_READ(GPIO_IN_REG) & (1UL << ENC_A_PIN); }
  static inline bool ISR_CODE encB() { return REG_READ(GPIO_IN_REG) & (1UL << ENC_B_PIN); }
#endif

static inline uint8_t ISR_CODE encRead() {
  return (encA() ? 0x02 : 0) | (encB() ? 0x01 : 0);
}

static void ISR_CODE encISR() {
  uint8_t cur  = encRead();
  uint8_t prev = encState;
  encState = cur;
  int8_t d = QUAD_TABLE[(prev << 2) | cur];
  if (d) {
    encCount = encCount + d;
  } else if (prev != cur) {
    encErrors = encErrors + 1;  // przeskok o dwa kroki - zgubione przerwanie
  }
}

// AVR czyta 32 bity w kilku instrukcjach - przerwanie w srodku daloby smieci.
static inline int32_t encNow() {
#if defined(ARDUINO_ARCH_AVR)
  noInterrupts();
  int32_t v = encCount;
  interrupts();
  return v;
#else
  return encCount;
#endif
}

static inline bool encTakeErrors() {
  noInterrupts();
  bool any = encErrors != 0;
  encErrors = 0;
  interrupts();
  return any;
}

// ------------------------------------------------------------------ przyciski

static const uint8_t BUTTON_PINS[] = { BUTTON_PIN_LIST };
#define BUTTON_COUNT ((uint8_t)sizeof(BUTTON_PINS))
static_assert(sizeof(BUTTON_PINS) <= MAX_BUTTONS, "najwyzej 32 przyciski");

static uint32_t btnStable = 0;    // stan po debounce (1 = wcisniety)
static uint32_t btnCnt0   = 0;
static uint32_t btnCnt1   = 0;

static inline uint32_t btnSampleRaw() {
  uint32_t s = 0;
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    if (digitalRead(BUTTON_PINS[i]) == LOW) s |= (1UL << i);
  }
  return s;
}

// Debounce wertykalnym licznikiem - 32 przyciski naraz, 2-bitowy licznik.
// Wywolywany co 1 ms -> zmiana stanu uznana po 3-4 ms stabilnosci.
static inline void btnDebounce(uint32_t sample) {
  uint32_t delta = sample ^ btnStable;
  btnCnt1 = (btnCnt1 ^ btnCnt0) & delta;
  btnCnt0 = (~btnCnt0) & delta;
  uint32_t toggle = delta & ~(btnCnt0 | btnCnt1);
  btnStable ^= toggle;
}

// ---------------------------------------------------------------- analogowe

static const uint8_t AXIS_PINS[NUM_AXES] = { AXIS_PIN_LIST };
static int32_t  adcAcc[NUM_AXES];        // akumulator filtru IIR
static uint16_t adcValue[NUM_AXES];      // wynik 0..4095
static uint8_t  adcNext = 0;             // nastepna os do odczytu

// Odczyt 0..4095 niezaleznie od rozdzielczosci ADC plytki.
static uint16_t adcRead(uint8_t pin) {
#if !defined(ARDUINO_ARCH_ESP32)
  analogRead(pin);                                  // odrzucamy probke po
#endif                                              // przelaczeniu kanalu
  return (uint16_t)analogRead(pin) << (12 - ADC_BITS);
}

static void adcInit() {
#if defined(ARDUINO_ARCH_SAM) || defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(ADC_BITS);
#endif
  for (uint8_t i = 0; i < NUM_AXES; i++) {
    uint16_t v = (AXIS_PINS[i] == PIN_NONE) ? 0 : adcRead(AXIS_PINS[i]);
    adcAcc[i]   = (int32_t)v << ADC_FILTER_SHIFT;
    adcValue[i] = v;
  }
}

// Kilka osi na tik - w AVR analogRead trwa ok. 110 us, wiec wszystkie
// szesc (z probka odrzucana) zajeloby ponad milisekunde.
static void adcUpdate() {
  for (uint8_t n = 0; n < AXES_PER_TICK; n++) {
    uint8_t i = adcNext;
    adcNext = (uint8_t)((adcNext + 1) % NUM_AXES);
    if (AXIS_PINS[i] == PIN_NONE) continue;
    int32_t raw = adcRead(AXIS_PINS[i]);
    adcAcc[i]  += raw - (adcAcc[i] >> ADC_FILTER_SHIFT);
    adcValue[i] = (uint16_t)(adcAcc[i] >> ADC_FILTER_SHIFT);
  }
}

// --------------------------------------------------------------------- FFB

static bool     ffbEnabled    = false;   // bezpieczny stan po restarcie
static int16_t  ffbTarget     = 0;       // zadany moment -32767..32767
static int16_t  ffbApplied    = 0;       // po ograniczeniu narastania
static uint8_t  ffbMinDuty    = 0;       // martwa strefa mostka (0..255)
static uint8_t  ffbMaxDuty    = 255;
static uint16_t ffbDeadband   = 300;     // ponizej -> zero momentu
static uint16_t ffbTimeoutMs  = 200;
static uint16_t ffbSlewPerMs  = 4000;    // max zmiana momentu na ms
static bool     ffbInvert     = false;
static uint32_t ffbLastCmdMs  = 0;
static bool     ffbWatchdogHit = false;

static int32_t  endstopCounts = 0;       // 0 = wylaczone
static uint16_t endstopGain   = 8;       // moment na jeden count przekroczenia
static bool     endstopHit    = false;

static void ffbOutput(int16_t torque) {
#if FFB_HARDWARE_PRESENT
  if (!ffbEnabled || torque == 0) {
    analogWrite(FFB_RPWM_PIN, 0);
    analogWrite(FFB_LPWM_PIN, 0);
    digitalWrite(FFB_EN_PIN, LOW);
    return;
  }
  if (ffbInvert) torque = -torque;

  uint32_t mag = (torque < 0) ? (uint32_t)(-(int32_t)torque) : (uint32_t)torque;
  // skalowanie 0..32767 -> ffbMinDuty..ffbMaxDuty
  uint32_t span = (uint32_t)(ffbMaxDuty - ffbMinDuty);
  uint8_t  duty = (uint8_t)(ffbMinDuty + (mag * span) / 32767UL);

  digitalWrite(FFB_EN_PIN, HIGH);
  if (torque > 0) {
    analogWrite(FFB_LPWM_PIN, 0);
    analogWrite(FFB_RPWM_PIN, duty);
  } else {
    analogWrite(FFB_RPWM_PIN, 0);
    analogWrite(FFB_LPWM_PIN, duty);
  }
#else
  (void)torque;
#endif
}

static void ffbUpdate(uint32_t nowMs, int32_t pos) {
  // 1. watchdog - brak komend z PC oznacza zwolnienie silnika
  if (ffbEnabled && (uint32_t)(nowMs - ffbLastCmdMs) > ffbTimeoutMs) {
    if (ffbTarget != 0) ffbWatchdogHit = true;
    ffbTarget = 0;
  }

  // 2. miekki ogranicznik konca zakresu - dziala nawet bez komend z PC
  int32_t combined = ffbTarget;
  endstopHit = false;
  if (endstopCounts > 0) {
    int32_t over = 0;
    if (pos >  endstopCounts) over = pos - endstopCounts;
    if (pos < -endstopCounts) over = pos + endstopCounts;
    if (over != 0) {
      endstopHit = true;
      int32_t push = -over * (int32_t)endstopGain;
      if (push >  32767) push =  32767;
      if (push < -32767) push = -32767;
      combined += push;
    }
  }
  if (combined >  32767) combined =  32767;
  if (combined < -32767) combined = -32767;

  // 3. martwa strefa
  if (combined > -(int32_t)ffbDeadband && combined < (int32_t)ffbDeadband) combined = 0;

  // 4. ograniczenie narastania - chroni mechanike i zasilacz
  int32_t diff = combined - ffbApplied;
  int32_t lim  = (int32_t)ffbSlewPerMs;
  if (diff >  lim) combined = ffbApplied + lim;
  if (diff < -lim) combined = ffbApplied - lim;

  ffbApplied = (int16_t)combined;
  ffbOutput(ffbApplied);
}

// ------------------------------------------------------------- wysylanie ramek

static uint8_t txBuf[MAX_PAYLOAD + 8];

static inline void putU8 (uint8_t *b, uint8_t  &i, uint8_t  v) { b[i++] = v; }
static inline void putU16(uint8_t *b, uint8_t  &i, uint16_t v) { b[i++]=v&0xFF; b[i++]=v>>8; }
static inline void putU32(uint8_t *b, uint8_t  &i, uint32_t v) {
  b[i++]=v&0xFF; b[i++]=(v>>8)&0xFF; b[i++]=(v>>16)&0xFF; b[i++]=(v>>24)&0xFF;
}

// Czy mozna pisac bez blokowania petli.
static inline bool txReady(uint8_t need) {
#if LINK_NATIVE_USB && defined(ARDUINO_ARCH_SAM)
  // SerialUSB Due: availableForWrite() zwraca stala, ale write() nic nie
  // robi, gdy host nie otworzyl portu - operator bool() sprawdza lineState.
  (void)need;
  return (bool)WPORT;
#elif LINK_NATIVE_USB && defined(ARDUINO_ARCH_AVR)
  // Leonardo: operator bool() czeka 10 ms - nie wolno go wolac w petli.
  // Bez otwartego portu write() i tak nic nie wysyla.
  return WPORT.availableForWrite() >= (int)need;
#elif LINK_NATIVE_USB
  return (bool)WPORT && WPORT.availableForWrite() >= (int)need;
#else
  // UART: wolne miejsce w buforze TX
  return WPORT.availableForWrite() >= (int)need;
#endif
}

// Zwraca false gdy nie ma miejsca - ramka jest wtedy POMIJANA.
// To jest kluczowe: nigdy nie blokujemy petli na write().
static bool sendFrame(uint8_t type, const uint8_t *payload, uint8_t len) {
  if (!txReady((uint8_t)(len + 5))) return false;

  uint8_t head[2] = { type, len };
  uint8_t c = crc8(head, 2);
  for (uint8_t i = 0; i < len; i++) c = crcStep(c, payload[i]);

  uint8_t frame[MAX_PAYLOAD + 5];
  uint8_t n = 0;
  frame[n++] = SYNC1;
  frame[n++] = SYNC2;
  frame[n++] = type;
  frame[n++] = len;
  for (uint8_t i = 0; i < len; i++) frame[n++] = payload[i];
  frame[n++] = c;
  WPORT.write(frame, n);
  return true;
}

static uint16_t reportHz  = DEFAULT_REPORT_HZ;
static uint32_t reportUs  = 1000000UL / DEFAULT_REPORT_HZ;
static uint16_t stateSeq  = 0;
static bool     encErrSeen = false;
static bool     hostSeen   = false;   // PC przyslal cokolwiek -> koniec HELLO

static void sendHello() {
  uint8_t i = 0;
  uint8_t caps = 0;
#if FFB_HARDWARE_PRESENT
  caps |= CAP_FFB;
#endif
#if LINK_NATIVE_USB
  caps |= CAP_NATIVE_USB;
#endif
#if HAS_G29
  caps |= CAP_G29;
#endif
#if G29_CAN_HIDE
  caps |= CAP_G29_HIDE;
#endif
  putU8 (txBuf, i, PROTO_VERSION);
  putU8 (txBuf, i, FW_MAJOR);
  putU8 (txBuf, i, FW_MINOR);
  putU8 (txBuf, i, caps);
  putU8 (txBuf, i, BUTTON_COUNT);
  putU8 (txBuf, i, NUM_AXES);
  putU16(txBuf, i, reportHz);
  putU32(txBuf, i, (uint32_t)endstopCounts);
  const char *p = BOARD_NAME;
  while (*p && i < MAX_PAYLOAD) txBuf[i++] = (uint8_t)*p++;
  sendFrame(MSG_HELLO, txBuf, i);
}

static void sendState(int32_t pos, uint32_t nowMs) {
  uint8_t i = 0;
  uint8_t flags = 0;
  if (ffbEnabled)      flags |= 0x01;
  if (ffbApplied != 0) flags |= 0x02;
  if (encErrSeen)      flags |= 0x04;
  if (endstopHit)      flags |= 0x08;
  if (ffbWatchdogHit)  flags |= 0x10;
#if HAS_G29
  if (g29Usb.hostConfigured()) flags |= 0x20;   // Windows wyliczyl G29
  if (g29Fed)                  flags |= 0x40;   // raporty z aplikacji docieraja
  if (g29Usb.attached())       flags |= 0x80;   // G29 podlaczona do komputera
#endif

  putU16(txBuf, i, stateSeq);
  putU32(txBuf, i, nowMs);
  putU32(txBuf, i, (uint32_t)pos);
  for (uint8_t a = 0; a < NUM_AXES; a++) putU16(txBuf, i, adcValue[a]);
  putU32(txBuf, i, btnStable);
  putU8 (txBuf, i, 0);          // hat - mapowany w aplikacji na PC
  putU8 (txBuf, i, flags);

  if (sendFrame(MSG_STATE, txBuf, i)) {
    stateSeq++;
    encErrSeen = false;
  }
}

static void sendAck(uint8_t type, uint8_t ok) {
  uint8_t p[2] = { type, ok };
  sendFrame(MSG_ACK, p, 2);
}

// -------------------------------------------------------------- odbior komend

static inline uint16_t getU16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t getU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void handleCommand(uint8_t type, const uint8_t *p, uint8_t len) {
  hostSeen = true;
  switch (type) {
    case CMD_PING: {
      if (len < 4) { sendAck(type, 0); return; }
      uint8_t i = 0;
      putU32(txBuf, i, getU32(p));
      putU32(txBuf, i, millis());
      sendFrame(MSG_PONG, txBuf, i);
      break;
    }
    case CMD_IDENTIFY:
      sendHello();
      break;

    case CMD_CENTER:
      encOffset = encNow();
      sendAck(type, 1);
      break;

    case CMD_SET_ENDSTOP: {
      if (len < 4) { sendAck(type, 0); return; }
      int32_t v = (int32_t)getU32(p);
      if (v < 0) v = 0;
      endstopCounts = v;
      if (len >= 6) {
        uint16_t g = getU16(p + 4);
        if (g > 0) endstopGain = g;
      }
      sendAck(type, 1);
      break;
    }
    case CMD_SET_RATE: {
      if (len < 2) { sendAck(type, 0); return; }
      uint16_t hz = getU16(p);
      if (hz < MIN_REPORT_HZ) hz = MIN_REPORT_HZ;
      if (hz > MAX_REPORT_HZ) hz = MAX_REPORT_HZ;
      reportHz = hz;
      reportUs = 1000000UL / hz;
      sendAck(type, 1);
      break;
    }
    case CMD_FFB: {
      if (len < 2) { sendAck(type, 0); return; }
      ffbTarget      = (int16_t)getU16(p);
      ffbLastCmdMs   = millis();
      ffbWatchdogHit = false;
      break;                      // bez ACK - idzie z duza czestotliwoscia
    }
    case CMD_FFB_CONFIG: {
      if (len < 9) { sendAck(type, 0); return; }
      ffbEnabled   = p[0] != 0;
      ffbMinDuty   = p[1];
      ffbMaxDuty   = p[2];
      if (ffbMaxDuty <= ffbMinDuty) ffbMaxDuty = 255;
      ffbInvert    = p[3] != 0;
      ffbDeadband  = getU16(p + 4);
      ffbTimeoutMs = getU16(p + 6);
      if (ffbTimeoutMs < 20) ffbTimeoutMs = 20;
      ffbSlewPerMs = (uint16_t)p[8] * 256;
      if (ffbSlewPerMs == 0) ffbSlewPerMs = 65535;
      ffbLastCmdMs = millis();
      if (!ffbEnabled) { ffbTarget = 0; ffbApplied = 0; ffbOutput(0); }
      sendAck(type, 1);
      break;
    }
#if HAS_G29
    case CMD_G29:
      if (g29Usb.setReport(p, len)) {
        g29LastCmdMs = millis();
        g29Fed = true;
      } else {
        sendAck(type, 0);
      }
      break;                      // bez ACK - idzie z kazda ramka
#endif
#if G29_CAN_HIDE
    case CMD_G29_USB:
      if (len < 1) { sendAck(type, 0); return; }
      g29Usb.setAttached(p[0] != 0);
      sendAck(type, 1);
      break;
#endif

    case CMD_RESET:
      ffbEnabled = false;
      ffbTarget  = 0;
      ffbApplied = 0;
      ffbOutput(0);
      encOffset  = encNow();
      sendAck(type, 1);
      sendHello();
      break;

    default:
      sendAck(type, 0);
      break;
  }
}

// Parser strumieniowy - odporny na utrate bajtow, sam sie resynchronizuje.
static void pollSerial() {
  static uint8_t  st = 0, rType = 0, rLen = 0, rIdx = 0;
  static uint8_t  rBuf[MAX_PAYLOAD];

  int budget = 64;                 // limit bajtow na jedno wywolanie
  while (budget-- > 0 && WPORT.available() > 0) {
    uint8_t b = (uint8_t)WPORT.read();
    switch (st) {
      case 0: if (b == SYNC1) st = 1; break;
      case 1: st = (b == SYNC2) ? 2 : ((b == SYNC1) ? 1 : 0); break;
      case 2: rType = b; st = 3; break;
      case 3:
        rLen = b;
        if (rLen > MAX_PAYLOAD) { st = 0; break; }
        rIdx = 0;
        st = (rLen == 0) ? 5 : 4;
        break;
      case 4:
        rBuf[rIdx++] = b;
        if (rIdx >= rLen) st = 5;
        break;
      case 5: {
        uint8_t head[2] = { rType, rLen };
        uint8_t c = crc8(head, 2);
        for (uint8_t i = 0; i < rLen; i++) c = crcStep(c, rBuf[i]);
        if (c == b) handleCommand(rType, rBuf, rLen);
        st = 0;
        break;
      }
    }
  }
}

// -------------------------------------------------------------------- setup

static void portBegin() {
#if WPORT_IS_TINYUSB
  // Tozsamosc USB (G29) i port COM trzeba zglosic przed USB.begin().
  #if HAS_G29
  g29Usb.begin();
  #endif
  wheelCdc.begin();
  USB.begin();
#elif LINK_NATIVE_USB
  WPORT.begin(0);
#else
  #if defined(ARDUINO_ARCH_ESP32)
  WPORT.setTxBufferSize(256);          // domyslnie tylko 128 B sprzetowej kolejki
  #endif
  WPORT.begin(WHEEL_BAUD);
#endif
}

void setup() {
  portBegin();

  // --- przyciski ---
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) pinMode(BUTTON_PINS[i], INPUT_PULLUP);

  // --- enkoder ---
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  encPinsInit();
  encState = encRead();
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encISR, CHANGE);
  encOffset = encNow();

  // --- analogowe ---
  adcInit();

  // --- FFB ---
#if FFB_HARDWARE_PRESENT
  #if defined(ARDUINO_ARCH_SAM)
  analogWriteResolution(8);
  #endif
  pinMode(FFB_EN_PIN, OUTPUT);
  digitalWrite(FFB_EN_PIN, LOW);
  pinMode(FFB_RPWM_PIN, OUTPUT);
  pinMode(FFB_LPWM_PIN, OUTPUT);
  analogWrite(FFB_RPWM_PIN, 0);
  analogWrite(FFB_LPWM_PIN, 0);
#endif

  // Stabilne poczatkowe odczyty przed pierwsza ramka - bez delay() w petli
  uint32_t t0 = millis();
  while (millis() - t0 < 50) { adcUpdate(); btnDebounce(btnSampleRaw()); }

  sendHello();
}

// --------------------------------------------------------------------- loop

void loop() {
  static uint32_t nextTick   = 0;
  static uint32_t nextReport = 0;
  static uint32_t nextHello  = 0;

  uint32_t now = micros();

  // 1. komendy z PC - zawsze, bez limitu czasowego
  pollSerial();

  // 2. zadania 1 kHz: debounce przyciskow, filtr ADC, FFB
  if ((int32_t)(now - nextTick) >= 0) {
    nextTick = now + (1000000UL / TICK_HZ);

    btnDebounce(btnSampleRaw());
    adcUpdate();

    if (encTakeErrors()) encErrSeen = true;

    int32_t pos = encNow() - encOffset;
    ffbUpdate(millis(), pos);

#if HAS_G29
    // aplikacja zamilkla - kierownica na srodek, pedaly puszczone
    if (g29Fed && (uint32_t)(millis() - g29LastCmdMs) > G29_TIMEOUT_MS) {
      g29Usb.setNeutral();
      g29Fed = false;
    }
#endif
  }

#if HAS_G29
  // 2b. raport G29 do Windowsa - tylko gdy endpoint jest wolny
  g29Usb.poll();
  {
    uint8_t out[G29_OUT_MAX];
    uint8_t n = g29Usb.takeOutput(out);
    if (n) sendFrame(MSG_G29_OUT, out, n);
  }
#endif

  // 3. ramka stanu w stalym tempie
  if ((int32_t)(now - nextReport) >= 0) {
    nextReport = now + reportUs;
    sendState(encNow() - encOffset, millis());
  }

  // 4. dopoki PC sie nie odezwal, co 1 s wysylamy HELLO (pomaga autodetekcji)
  if (!hostSeen && (int32_t)(now - nextHello) >= 0) {
    nextHello = now + 1000000UL;
    sendHello();
  }
}

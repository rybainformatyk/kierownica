/* =============================================================================
 *  KIEROWNICA  -  firmware dla Arduino Due
 *  Wersja 2.1  -  lacze szeregowe z aplikacja + kierownica G29 na Native USB
 * =============================================================================
 *
 *  NOWE W 2.1 - KIEROWNICA G29
 *    Port Native USB przedstawia sie Windowsowi jako Logitech G29 (VID 046D,
 *    PID C24F, oryginalny uklad raportu) - patrz g29_usb.h. Gry, ktore
 *    przyjmuja tylko kierownice ze swojej listy (Forza), widza wtedy G29.
 *    Kalibracje i mapowanie nadal robi aplikacja: liczy gotowy raport G29 i
 *    przysyla go komenda CMD_G29, a plytka tylko wypycha go przez USB.
 *    Podlacz OBA kable: Programming Port (lacze z aplikacja) i Native USB
 *    (kierownica dla gier). Gdy aplikacja zamilknie na 250 ms, G29 wraca do
 *    pozycji spoczynkowej - auto nie zostanie z wcisnietym gazem.
 *
 *  Plytka NIE udaje juz joysticka. Wysyla surowe odczyty ramkami binarnymi
 *  do aplikacji "panel" na PC, a ta tworzy wirtualne urzadzenie (vJoy),
 *  ktore gry widza jako kierownice.
 *
 *  Co zostalo naprawione wzgledem wersji 1 (HID, bez aplikacji na PC):
 *    * Serial 9600 + dlugi debug co 10 ms zapychal bufor TX i BLOKOWAL petle
 *      (to bylo zacinanie sie) -> teraz 250000 baud + ramki 33 B + brak
 *      zapisu gdy bufor pelny.
 *    * delay(10) -> nieblokujacy harmonogram na micros().
 *    * map() bez constrain() wychodzil poza zakres osi -> skalowanie i
 *      kalibracja przeniesione do aplikacji na PC (zmiana bez reflashowania).
 *    * Encoder.h -> wlasny dekoder kwadraturowy x4 na przerwaniach (bez
 *      zaleznosci, obsluga bledow kwadratury).
 *    * ADC 10-bit -> 12-bit (4096 krokow) + filtr IIR + odrzucenie pierwszej
 *      probki po przelaczeniu kanalu.
 *    * brak debounce -> debounce wertykalnym licznikiem na wszystkich 32
 *      przyciskach jednoczesnie.
 *    * parser komend "send 123" rozjezdzal sie przy bledzie -> ramki z CRC8
 *      i automatyczna resynchronizacja.
 *    * dodany kanal Force Feedback z watchdogiem (wylaczony po starcie).
 *
 *  POLACZENIE
 *    Domyslnie uzywany jest PROGRAMMING PORT (gniazdo przy zlaczu zasilania),
 *    250000 baud - ta wartosc dzieli sie calkowicie zarowno przez zegar
 *    SAM3X (84 MHz / 16 / 21) jak i ATmega16U2 (16 MHz / 8 / 8), wiec nie ma
 *    bledu zegara. Zapas pasma przy 333 Hz to ponad 2x.
 *    Aby uzyc NATIVE USB PORT (szybszy, mniejsze opoznienie) ustaw nizej
 *    WHEEL_USE_NATIVE_USB na 1 i wybierz w aplikacji ten drugi port COM.
 *
 *  PINY (zgodne z Twoim ukladem)
 *    2, 3        - enkoder kwadraturowy A / B
 *    A0..A5      - potencjometry: gaz, hamulec, sprzeglo, aux1, aux2, aux3
 *    4           - przycisk nr 0
 *    22..52      - przyciski nr 1..31
 *    5, 6, 7     - opcjonalny mostek H do FFB (EN, RPWM, LPWM) - BTS7960/IBT-2
 * =============================================================================
 */

// ---------------------------------------------------------------- konfiguracja

// #ifndef pozwala nadpisac ustawienie z linii polecen kompilatora (-D...)
#ifndef WHEEL_USE_NATIVE_USB
#define WHEEL_USE_NATIVE_USB 0      // 0 = Programming Port, 1 = Native USB Port
#endif
#define WHEEL_BAUD           250000 // ignorowane przy Native USB
#ifndef WHEEL_G29_USB
#define WHEEL_G29_USB        1      // 1 = Native USB udaje kierownice Logitech G29
#endif
#define G29_TIMEOUT_MS       250    // brak raportu z aplikacji -> pozycja spoczynkowa

#define ENC_A_PIN            2
#define ENC_B_PIN            3

#define BUTTON0_PIN          4      // pojedynczy przycisk -> bit 0
#define BUTTON_BANK_FIRST    22     // 22..52 -> bity 1..31
#define BUTTON_BANK_LAST     52

#define FFB_EN_PIN           5
#define FFB_RPWM_PIN         6
#define FFB_LPWM_PIN         7
#define FFB_HARDWARE_PRESENT 1      // 0 = nie ruszaj tych pinow wcale

#define TICK_HZ              1000   // czestotliwosc filtrow i debounce
#define DEFAULT_REPORT_HZ    333    // czestotliwosc ramek stanu
#define MIN_REPORT_HZ        50
#define MAX_REPORT_HZ        500

#define ADC_FILTER_SHIFT     3      // filtr IIR: alfa = 1/8

// --------------------------------------------------------------------- wybor portu

#if WHEEL_USE_NATIVE_USB
  #define WPORT SerialUSB
#else
  #define WPORT Serial
#endif

// --------------------------------------------------------------------- protokol

#define PROTO_VERSION  1
#define FW_MAJOR       2
#define FW_MINOR       1
#define DEVICE_NAME    "KIEROWNICA-DUE"

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

#define CAP_FFB        0x01
#define CAP_NATIVE_USB 0x02
#define CAP_G29        0x04

#define NUM_AXES    6
#define NUM_BUTTONS 32

#define MAX_PAYLOAD 64

// ----------------------------------------------------------------------- G29

#if WHEEL_G29_USB
  #include "g29_usb.h"
  // Obiekt globalny: konstruktor rejestruje interfejs USB, zanim rdzen
  // podlaczy port Native USB do hosta.
  static G29Usb g29Usb;
  static uint32_t g29LastCmdMs = 0;
  static bool     g29Fed       = false;   // aplikacja przysyla raporty
#endif

// ---------------------------------------------------------------------- CRC8

// CRC-8/ATM, polinom 0x07, init 0x00 - tablica w flashu
static const uint8_t CRC8_TABLE[256] = {
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

static inline uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t c = 0x00;
  while (len--) c = CRC8_TABLE[c ^ *data++];
  return c;
}

// ------------------------------------------------------------------- enkoder

// Dekoder kwadraturowy x4. Indeks = (poprzedni_stan << 2) | nowy_stan,
// gdzie stan = (A << 1) | B. Zera na przekatnej, 0 przy przeskoku o 2 (blad).
static const int8_t QUAD_TABLE[16] = {
   0, +1, -1,  0,
  -1,  0,  0, +1,
  +1,  0,  0, -1,
   0, -1, +1,  0
};

volatile int32_t  encCount    = 0;
volatile uint32_t encErrors   = 0;
volatile uint8_t  encState    = 0;

static Pio      *encAPort, *encBPort;
static uint32_t  encAMask,  encBMask;

static int32_t   encOffset = 0;   // pozycja zera (ustawiana przez CMD_CENTER)

static inline uint8_t encRead() {
  uint8_t s = 0;
  if (encAPort->PIO_PDSR & encAMask) s |= 0x02;
  if (encBPort->PIO_PDSR & encBMask) s |= 0x01;
  return s;
}

static void encISR() {
  uint8_t cur  = encRead();
  uint8_t prev = encState;
  encState = cur;
  int8_t d = QUAD_TABLE[(prev << 2) | cur];
  if (d) {
    encCount += d;
  } else if (prev != cur) {
    encErrors++;              // przeskok o dwa kroki - zgubione przerwanie
  }
}

// ------------------------------------------------------------------ przyciski

static const uint8_t BANK_COUNT = BUTTON_BANK_LAST - BUTTON_BANK_FIRST + 1; // 31

static uint32_t btnStable = 0;    // stan po debounce (1 = wcisniety)
static uint32_t btnCnt0   = 0;
static uint32_t btnCnt1   = 0;

static inline uint32_t btnSampleRaw() {
  uint32_t s = 0;
  if (digitalRead(BUTTON0_PIN) == LOW) s |= 1UL;
  for (uint8_t i = 0; i < BANK_COUNT; i++) {
    if (digitalRead(BUTTON_BANK_FIRST + i) == LOW) s |= (1UL << (i + 1));
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

static const uint8_t ADC_PINS[NUM_AXES] = { A0, A1, A2, A3, A4, A5 };
static int32_t  adcAcc[NUM_AXES];        // akumulator filtru IIR
static uint16_t adcValue[NUM_AXES];      // wynik 0..4095

static void adcInit() {
  analogReadResolution(12);
  for (uint8_t i = 0; i < NUM_AXES; i++) {
    analogRead(ADC_PINS[i]);                       // rozgrzanie kanalu
    uint16_t v = analogRead(ADC_PINS[i]);
    adcAcc[i]   = (int32_t)v << ADC_FILTER_SHIFT;
    adcValue[i] = v;
  }
}

static void adcUpdate() {
  for (uint8_t i = 0; i < NUM_AXES; i++) {
    analogRead(ADC_PINS[i]);                       // odrzucamy probke po
    int32_t raw = analogRead(ADC_PINS[i]);         // przelaczeniu kanalu
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
// UART: patrzymy na wolne miejsce w buforze TX.
// Native USB: Serial_::availableForWrite() zwraca stala (bezuzyteczne), ale
// Serial_::write() nic nie robi gdy host nie otworzyl portu - wiec sprawdzamy
// operator bool(), ktory testuje lineState.
static inline bool txReady(uint8_t need) {
#if WHEEL_USE_NATIVE_USB
  (void)need;
  return (bool)WPORT;
#else
  return WPORT.availableForWrite() >= (int)need;
#endif
}

// Zwraca false gdy nie ma miejsca - ramka jest wtedy POMIJANA.
// To jest kluczowe: nigdy nie blokujemy petli na write().
static bool sendFrame(uint8_t type, const uint8_t *payload, uint8_t len) {
  if (!txReady((uint8_t)(len + 5))) return false;

  uint8_t head[2] = { type, len };
  uint8_t c = crc8(head, 2);
  for (uint8_t i = 0; i < len; i++) c = CRC8_TABLE[c ^ payload[i]];

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
#if WHEEL_USE_NATIVE_USB
  caps |= CAP_NATIVE_USB;
#endif
#if WHEEL_G29_USB
  caps |= CAP_G29;
#endif
  putU8 (txBuf, i, PROTO_VERSION);
  putU8 (txBuf, i, FW_MAJOR);
  putU8 (txBuf, i, FW_MINOR);
  putU8 (txBuf, i, caps);
  putU8 (txBuf, i, NUM_BUTTONS);
  putU8 (txBuf, i, NUM_AXES);
  putU16(txBuf, i, reportHz);
  putU32(txBuf, i, (uint32_t)endstopCounts);
  const char *p = DEVICE_NAME;
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
#if WHEEL_G29_USB
  if (g29Usb.hostConfigured()) flags |= 0x20;   // Windows wyliczyl G29
  if (g29Fed)                  flags |= 0x40;   // raporty z aplikacji docieraja
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
      encOffset = encCount;
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
#if WHEEL_G29_USB
    case CMD_G29:
      if (g29Usb.setReport(p, len)) {
        g29LastCmdMs = millis();
        g29Fed = true;
      } else {
        sendAck(type, 0);
      }
      break;                      // bez ACK - idzie z kazda ramka
#endif

    case CMD_RESET:
      ffbEnabled = false;
      ffbTarget  = 0;
      ffbApplied = 0;
      ffbOutput(0);
      encOffset  = encCount;
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
        for (uint8_t i = 0; i < rLen; i++) c = CRC8_TABLE[c ^ rBuf[i]];
        if (c == b) handleCommand(rType, rBuf, rLen);
        st = 0;
        break;
      }
    }
  }
}

// -------------------------------------------------------------------- setup

void setup() {
#if WHEEL_USE_NATIVE_USB
  WPORT.begin(0);
#else
  WPORT.begin(WHEEL_BAUD);
#endif

  // --- przyciski ---
  pinMode(BUTTON0_PIN, INPUT_PULLUP);
  for (uint8_t pin = BUTTON_BANK_FIRST; pin <= BUTTON_BANK_LAST; pin++) {
    pinMode(pin, INPUT_PULLUP);
  }

  // --- enkoder ---
  pinMode(ENC_A_PIN, INPUT_PULLUP);
  pinMode(ENC_B_PIN, INPUT_PULLUP);
  encAPort = g_APinDescription[ENC_A_PIN].pPort;
  encAMask = g_APinDescription[ENC_A_PIN].ulPin;
  encBPort = g_APinDescription[ENC_B_PIN].pPort;
  encBMask = g_APinDescription[ENC_B_PIN].ulPin;
  encState = encRead();
  attachInterrupt(digitalPinToInterrupt(ENC_A_PIN), encISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B_PIN), encISR, CHANGE);
  encOffset = encCount;

  // --- analogowe ---
  adcInit();

  // --- FFB ---
#if FFB_HARDWARE_PRESENT
  analogWriteResolution(8);
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

    if (encErrors) { encErrors = 0; encErrSeen = true; }

    int32_t pos = encCount - encOffset;
    ffbUpdate(millis(), pos);

#if WHEEL_G29_USB
    // aplikacja zamilkla - kierownica na srodek, pedaly puszczone
    if (g29Fed && (uint32_t)(millis() - g29LastCmdMs) > G29_TIMEOUT_MS) {
      g29Usb.setNeutral();
      g29Fed = false;
    }
#endif
  }

#if WHEEL_G29_USB
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
    sendState(encCount - encOffset, millis());
  }

  // 4. dopoki PC sie nie odezwal, co 1 s wysylamy HELLO (pomaga autodetekcji)
  if (!hostSeen && (int32_t)(now - nextHello) >= 0) {
    nextHello = now + 1000000UL;
    sendHello();
  }
}

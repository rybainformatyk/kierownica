/* =============================================================================
 *  PLYTKI I PINY
 * =============================================================================
 *
 *  Ten sam firmware dziala na kilku plytkach. Plytka jest wybierana sama,
 *  na podstawie tego, dla czego kompiluje Arduino IDE / arduino-cli.
 *  Tu sa wszystkie roznice miedzy plytkami: piny, rozdzielczosc ADC, port
 *  lacza z aplikacja i to, czy plytka umie udawac kierownice G29.
 *
 *  Plytka                      | lacze z PC          | G29 | przyciski | ADC
 *  ----------------------------+---------------------+-----+-----------+------
 *  Arduino Due                 | Programming Port    | tak |    32     | 12 b
 *  Arduino Mega 2560           | USB (ATmega16U2)    | nie |    32     | 10 b
 *  Arduino Uno / Nano          | USB (16U2 / CH340)  | nie |     7     | 10 b
 *  Arduino Leonardo / Micro    | natywne USB         | tak |    12     | 10 b
 *  ESP32 (WROOM, DevKit)       | USB (CP210x / CH340)| nie |    10     | 12 b
 *  ESP32-S3 (DevKitC-1)        | natywne USB (OTG)   | tak |    13     | 12 b
 *
 *  Plytki bez G29 dzialaja w trybach vJoy i pad Xbox. Odczyty 10-bitowe sa
 *  mnozone x4, zeby aplikacja zawsze dostawala zakres 0..4095.
 *
 *  Kazdy przycisk zwiera pin do GND (wewnetrzny pull-up). Pedaly
 *  i osie dodatkowe to potencjometry miedzy GND a zasilaniem plytki
 *  (5 V w AVR, 3,3 V w Due i ESP32 - NIE podawaj 5 V na piny Due ani ESP32).
 *
 *  Kolejnosc osi: gaz, hamulec, sprzeglo, aux1, aux2, aux3.
 *  Mostek H do FFB: BTS7960 / IBT-2 (EN, RPWM, LPWM).
 * =============================================================================
 */

#pragma once

#include <Arduino.h>

#define PIN_NONE 0xFF   // os bez potencjometru - wysylane jest 0

// ------------------------------------------------------------------ Arduino Due
#if defined(ARDUINO_SAM_DUE)

  #define BOARD_NAME        "KIEROWNICA-DUE"
  #define ENC_A_PIN         2
  #define ENC_B_PIN         3
  // 4, potem 22..52 - jak w poprzednich wersjach firmware
  #define BUTTON_PIN_LIST   4, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, \
                            37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52
  #define AXIS_PIN_LIST     A0, A1, A2, A3, A4, A5
  #define FFB_EN_PIN        5
  #define FFB_RPWM_PIN      6
  #define FFB_LPWM_PIN      7
  #define ADC_BITS          12
  #define AXES_PER_TICK     6     // wszystkie osie co 1 ms
  #define ADC_FILTER_SHIFT  3     // filtr IIR: alfa = 1/8

  // Lacze: Programming Port (0) albo Native USB (1). Przy 1 port Native USB
  // jest jednoczesnie laczem i G29, wiec G29 nie da sie odlaczyc.
  #ifndef WHEEL_USE_NATIVE_USB
  #define WHEEL_USE_NATIVE_USB 0
  #endif
  #if WHEEL_USE_NATIVE_USB
    #define WPORT           SerialUSB
    #define LINK_NATIVE_USB 1
  #else
    #define WPORT           Serial
    #define LINK_NATIVE_USB 0
  #endif
  #ifndef WHEEL_G29_USB
  #define WHEEL_G29_USB     1
  #endif
  #define G29_PLUGGABLE     WHEEL_G29_USB
  #define G29_CAN_HIDE      (WHEEL_G29_USB && !WHEEL_USE_NATIVE_USB)

// ------------------------------------------------------------ Arduino Mega 2560
#elif defined(__AVR_ATmega2560__)

  #define BOARD_NAME        "KIEROWNICA-MEGA"
  #define ENC_A_PIN         2     // INT4
  #define ENC_B_PIN         3     // INT5
  // ten sam uklad co w Due - plytki sa wymienne bez przelutowania
  #define BUTTON_PIN_LIST   4, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, \
                            37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52
  #define AXIS_PIN_LIST     A0, A1, A2, A3, A4, A5
  #define FFB_EN_PIN        5
  #define FFB_RPWM_PIN      6
  #define FFB_LPWM_PIN      7
  #define ADC_BITS          10
  #define AXES_PER_TICK     2     // analogRead trwa ok. 110 us - 2 osie na 1 ms
  #define ADC_FILTER_SHIFT  2     // os odswiezana co 3 ms - krotszy filtr
  #define WPORT             Serial
  #define LINK_NATIVE_USB   0

// ----------------------------------------------- Arduino Leonardo / Micro (32U4)
#elif defined(__AVR_ATmega32U4__)

  // Jedno gniazdo USB jest naraz laczem z aplikacja (port COM) i kierownica
  // G29. Pro Micro nie ma wyprowadzonych pinow 11-13 ani A4-A5 - te przyciski
  // i osie po prostu nie beda uzywane.
  #define BOARD_NAME        "KIEROWNICA-LEONARDO"
  #define ENC_A_PIN         2     // INT1
  #define ENC_B_PIN         3     // INT0
  // 0 i 1 sa wolne - lacze idzie przez USB, nie przez Serial1;
  // 14-16 to MISO, SCK, MOSI (zlacze ICSP w Leonardo, piny w Pro Micro)
  #define BUTTON_PIN_LIST   4, 5, 6, 7, 11, 12, 13, 0, 1, 14, 15, 16
  #define AXIS_PIN_LIST     A0, A1, A2, A3, A4, A5
  #define FFB_EN_PIN        8
  #define FFB_RPWM_PIN      9
  #define FFB_LPWM_PIN      10
  #define ADC_BITS          10
  #define AXES_PER_TICK     2
  #define ADC_FILTER_SHIFT  2
  #define WPORT             Serial
  #define LINK_NATIVE_USB   1
  #ifndef WHEEL_G29_USB
  #define WHEEL_G29_USB     1
  #endif
  #define G29_PLUGGABLE     WHEEL_G29_USB
  #define G29_CAN_HIDE      0     // odlaczenie USB zerwaloby lacze z aplikacja

// --------------------------------------------- Arduino Uno / Nano (ATmega328P)
#elif defined(__AVR_ATmega328P__)

  #define BOARD_NAME        "KIEROWNICA-UNO"
  #define ENC_A_PIN         2     // INT0
  #define ENC_B_PIN         3     // INT1
  // 0 i 1 to lacze z PC. Wiecej przyciskow: ustaw UNO_OSIE na 3 -
  // A3, A4, A5 zamienia sie w przyciski 7, 8, 9.
  #ifndef UNO_OSIE
  #define UNO_OSIE          6
  #endif
  #if UNO_OSIE == 3
    #define BUTTON_PIN_LIST 4, 5, 6, 7, 11, 12, 13, A3, A4, A5
    #define AXIS_PIN_LIST   A0, A1, A2, PIN_NONE, PIN_NONE, PIN_NONE
  #else
    #define BUTTON_PIN_LIST 4, 5, 6, 7, 11, 12, 13
    #define AXIS_PIN_LIST   A0, A1, A2, A3, A4, A5
  #endif
  #define FFB_EN_PIN        8
  #define FFB_RPWM_PIN      9     // Timer1 - nie rusza millis()
  #define FFB_LPWM_PIN      10
  #define ADC_BITS          10
  #define AXES_PER_TICK     2
  #define ADC_FILTER_SHIFT  2
  #define WPORT             Serial
  #define LINK_NATIVE_USB   0

// --------------------------------------------------------- ESP32-S3 / ESP32-S2
#elif defined(CONFIG_IDF_TARGET_ESP32S3) || defined(CONFIG_IDF_TARGET_ESP32S2)

  // Piny dla ESP32-S3-DevKitC-1. Pominiete: 0, 3, 45, 46 (piny startowe),
  // 19, 20 (USB), 26-37 (pamiec flash / PSRAM w modulach N8R8).
  #define BOARD_NAME        "KIEROWNICA-ESP32S3"
  #define ENC_A_PIN         15
  #define ENC_B_PIN         16
  #define BUTTON_PIN_LIST   1, 2, 13, 14, 17, 18, 21, 38, 39, 40, 41, 42, 47
  #define AXIS_PIN_LIST     4, 5, 6, 7, 8, 9    // ADC1 - dziala tez przy wlaczonym Wi-Fi
  #define FFB_EN_PIN        10
  #define FFB_RPWM_PIN      11
  #define FFB_LPWM_PIN      12
  #define ADC_BITS          12
  #define AXES_PER_TICK     3
  #define ADC_FILTER_SHIFT  3

  // "USB Mode: USB-OTG (TinyUSB)" - gniazdo USB (nie UART) jest laczem
  // z aplikacja i kierownica G29. W trybie "Hardware CDC" G29 nie ma.
  #if defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE == 0
    #ifndef WHEEL_G29_USB
    #define WHEEL_G29_USB   1
    #endif
    #define G29_TINYUSB     WHEEL_G29_USB
    #define WPORT           wheelCdc
    #define WPORT_IS_TINYUSB 1
    #define LINK_NATIVE_USB 1
  #else
    #define WPORT           Serial
    #define LINK_NATIVE_USB 1
  #endif
  #define G29_CAN_HIDE      0

// ----------------------------------------------------------- ESP32 (klasyczny)
#elif defined(CONFIG_IDF_TARGET_ESP32)

  // Piny dla ESP32 DevKit (WROOM-32). Pominiete: 0, 2, 12, 15 (piny
  // startowe), 6-11 (flash), 1, 3 (lacze z PC). 34-39 nie maja pull-upow,
  // wiec sa tylko osiami. W modulach WROVER 16 i 17 zajmuje PSRAM.
  #define BOARD_NAME        "KIEROWNICA-ESP32"
  #define ENC_A_PIN         18
  #define ENC_B_PIN         19
  #define BUTTON_PIN_LIST   4, 5, 13, 14, 16, 17, 21, 22, 23, 33
  #define AXIS_PIN_LIST     36, 39, 34, 35, 32, PIN_NONE   // ADC1
  #define FFB_EN_PIN        25
  #define FFB_RPWM_PIN      26
  #define FFB_LPWM_PIN      27
  #define ADC_BITS          12
  #define AXES_PER_TICK     3
  #define ADC_FILTER_SHIFT  3
  #define WPORT             Serial
  #define LINK_NATIVE_USB   0

#else
  #error "Nieobslugiwana plytka. Wybierz: Arduino Due, Mega 2560, Uno, Nano, Leonardo, Micro, ESP32 albo ESP32-S3."
#endif

// --------------------------------------------------------- wartosci domyslne

#ifndef G29_PLUGGABLE
#define G29_PLUGGABLE 0
#endif
#ifndef G29_TINYUSB
#define G29_TINYUSB 0
#endif
#ifndef G29_CAN_HIDE
#define G29_CAN_HIDE 0
#endif
#ifndef WPORT_IS_TINYUSB
#define WPORT_IS_TINYUSB 0
#endif
#define HAS_G29 (G29_PLUGGABLE || G29_TINYUSB)

#ifndef FFB_HARDWARE_PRESENT
#define FFB_HARDWARE_PRESENT 1      // 0 = nie ruszaj pinow mostka wcale
#endif

// Przetwarzanie surowych odczytow z plytki na znormalizowane osie.
//
// Wszystko co dotyczy kalibracji dzieje sie tutaj - plytka wysyla wylacznie
// surowe liczby, wiec kazda zmiana ustawien dziala natychmiast, bez
// programowania Arduino.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "config.h"
#include "outputs/xinput.h"
#include "protocol.h"

namespace wb {

// Krzywe jako czyste funkcje: liczy nimi przetwarzanie ramek i rysuje je
// podglad w panelu - wykres nie moze sie rozjechac z tym, co dostaje gra.
double pedal_position(int raw, const PedalConfig& cfg);
double pedal_curve(double value, const PedalConfig& cfg);
double steer_curve(double axis, double linearity);

// Surowy ADC -> 0..1 z kalibracja, martwa strefa, krzywa i filtrem.
class PedalProcessor {
public:
    void reset();
    double process(int raw, const PedalConfig& cfg);

private:
    double smoothed_ = 0.0;
    bool primed_ = false;
};

// Impulsy enkodera -> stopnie i os -1..1, z predkoscia katowa.
class SteerProcessor {
public:
    void reset();
    double velocity_deg_s() const { return velocity_; }
    // Zwraca (stopnie, os -1..1).
    std::pair<double, double> process(int32_t counts, const SteerConfig& cfg,
                                      std::optional<double> now = std::nullopt);

private:
    double smoothed_deg_ = 0.0;
    bool primed_ = false;
    double last_deg_ = 0.0;
    double last_t_ = 0.0;
    double velocity_ = 0.0;  // stopnie / s (wygladzone)
};

// Gotowy do wyslania wynik przetworzenia jednej ramki.
struct ProcessedState {
    double steer_deg = 0.0;
    double steer_axis = 0.0;      // -1..1
    double steer_velocity = 0.0;  // stopnie / s
    std::array<double, NUM_PEDALS> pedals{};
    std::array<uint16_t, protocol::NUM_AXES> raw_axes{};
    int32_t raw_counts = 0;
    uint32_t buttons_raw = 0;        // maska 32 bitow prosto z plytki
    uint32_t buttons_effective = 0;  // po uwzglednieniu inwersji (styk NC)
    std::array<uint8_t, protocol::NUM_BUTTONS> out_buttons{};
    int out_buttons_count = 0;
    int hat = -1;  // -1 = srodek, 0..3 = gora/prawo/dol/lewo
    uint8_t flags = 0;
    uint16_t seq = 0;

    double pedal(std::string_view key) const { return pedals[static_cast<size_t>(pedal_index(key))]; }
};

// Spaja procesory osi i mapowanie przyciskow w jedna operacje.
class StateProcessor {
public:
    void reset();
    ProcessedState process(const protocol::StateFrame& frame, const AppConfig& cfg,
                           std::optional<double> now = std::nullopt);

    SteerProcessor steer;
    std::array<PedalProcessor, NUM_PEDALS> pedals;
};

// Wartosci wszystkich zrodel pada, kazda 0..1.
std::map<std::string, double> pad_sources(const ProcessedState& state, const AppConfig& cfg);

// Zamienia przetworzony stan kierownicy na raport pada Xbox 360.
PadState build_pad_state(const ProcessedState& state, const AppConfig& cfg);

inline constexpr int G29_HAT_CENTER = 8;

// Stan kierownicy -> 12-bajtowy raport G29 (uklad w g29_usb.h).
protocol::G29Report build_g29_report(const ProcessedState& state, const AppConfig& cfg);

// Odwrotnosc build_g29_report dla przyciskow: co jest wcisniete w raporcie.
std::map<std::string, bool> g29_active(const protocol::G29Report& report);

} // namespace wb

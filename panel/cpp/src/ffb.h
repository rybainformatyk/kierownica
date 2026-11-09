// Force Feedback: odbior efektow z gry przez vJoy i zamiana ich na moment
// obrotowy wysylany do plytki.
//
// Gra wysyla do urzadzenia vJoy pakiety HID PID. vJoy przekazuje je nam przez
// callback FfbRegisterGenCB. Rozkladamy je funkcjami pomocniczymi Ffb_h_*
// z vJoyInterface.dll, utrzymujemy tablice aktywnych efektow i co `update_hz`
// razy na sekunde sumujemy ich wklad.
//
// BEZPIECZENSTWO
//   * kazda wartosc z DLL jest sprawdzana zakresem (+-10000); wartosci poza
//     zakresem traktujemy jako blad parsowania i ignorujemy - uszkodzony
//     pakiet nie moze wystrzelic silnikiem,
//   * callback nigdy nie rzuca wyjatku do kodu C,
//   * firmware ma wlasny watchdog: brak komend = zwolnienie silnika.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace wb {

class VJoyApi;

using LogFn = std::function<void(const std::string& level, const std::string& msg)>;

// typy pakietow (FFBPType z public.h)
inline constexpr int PT_EFFREP = 0x01;   // parametry efektu (typ, czas trwania, kierunek)
inline constexpr int PT_ENVREP = 0x02;   // obwiednia
inline constexpr int PT_CONDREP = 0x03;  // efekt warunkowy (spring / damper / inertia / friction)
inline constexpr int PT_PRIDREP = 0x04;  // efekt okresowy
inline constexpr int PT_CONSTREP = 0x05; // sila stala
inline constexpr int PT_RAMPREP = 0x06;  // narastanie
inline constexpr int PT_EFOPREP = 0x0A;  // start / stop efektu
inline constexpr int PT_BLKFRREP = 0x0B; // zwolnienie bloku efektu
inline constexpr int PT_CTRLREP = 0x0C;  // sterowanie urzadzeniem
inline constexpr int PT_GAINREP = 0x0D;  // globalne wzmocnienie urzadzenia

// typy efektow (FFBEType)
enum EffectType {
    ET_NONE = 0, ET_CONST, ET_RAMP, ET_SQR, ET_SINE, ET_TRNGL, ET_STUP, ET_STDN,
    ET_SPRNG, ET_DMPR, ET_INRT, ET_FRCTN, ET_CSTM
};

const char* effect_name(int type);

// operacje na efekcie (FFBOP)
inline constexpr int EFF_START = 1;
inline constexpr int EFF_SOLO = 2;
inline constexpr int EFF_STOP = 3;

// sterowanie urzadzeniem (FFB_CTRL)
inline constexpr int CTRL_ENACT = 1;
inline constexpr int CTRL_DISACT = 2;
inline constexpr int CTRL_STOPALL = 3;
inline constexpr int CTRL_DEVRST = 4;
inline constexpr int CTRL_DEVPAUSE = 5;
inline constexpr int CTRL_DEVCONT = 6;

extern const std::map<std::string, std::string> DIRECTION_LABELS;

struct Effect {
    int index = 0;
    int type = ET_NONE;
    double gain = 1.0;
    int duration_ms = 0;     // 0 = nieskonczony
    int direction = 0;       // 0..255
    bool polar = true;
    // stala sila / narastanie
    double magnitude = 0.0;  // -1..1
    double ramp_start = 0.0;
    double ramp_end = 0.0;
    // okresowy
    int period_ms = 100;
    double phase_deg = 0.0;
    double offset = 0.0;
    double amplitude = 0.0;
    // warunkowy
    double center = 0.0;
    double pos_coeff = 0.0;
    double neg_coeff = 0.0;
    double pos_sat = 1.0;
    double neg_sat = 1.0;
    double deadband = 0.0;
    // obwiednia
    double attack_level = 0.0;
    int attack_ms = 0;
    double fade_level = 0.0;
    int fade_ms = 0;
    // stan
    bool active = false;
    double start_time = 0.0;
    int loop_count = 1;

    void reset_state() {
        active = false;
        start_time = 0.0;
    }
};

// Skalowanie kategorii efektow z gry.
struct FfbGains {
    double constant = 1.0;
    double periodic = 1.0;
    double spring = 1.0;
    double damper = 1.0;
};

// Tlumaczy efekty FFB z gry na pojedynczy moment obrotowy -1..1.
class FfbManager {
public:
    static constexpr int MAX_EFFECTS = 32;

    explicit FfbManager(LogFn on_log = nullptr);

    // Podlacza callback vJoy. False, gdy DLL nie ma API FFB.
    bool register_callback(VJoyApi* api, int device_id);
    // vJoy nie ma funkcji odrejestrowania - przestajemy reagowac.
    void unregister();
    bool registered() const { return registered_.load(); }

    void reset();
    std::vector<std::tuple<int, std::string, double>> active_effects() const;
    double device_gain() const;
    void set_direction_mode(const std::string& mode);

    // Sumuje efekty gry. position -1..1, velocity_norm -1..1 -> moment -1..1.
    double compute(double position, double velocity_norm, const FfbGains& gains = {},
                   std::optional<double> now = std::nullopt);

    // Rozklada pakiet; uzywane przez callback vJoy.
    void on_packet(const void* packet);

    // Wejscie dla testow i callbacku - stan po zdekodowaniu pakietu.
    void store_effect_for_test(const Effect& effect);

    std::atomic<uint64_t> packets{0};
    std::atomic<uint64_t> parse_errors{0};
    std::atomic<double> last_packet_time{0.0};
    std::atomic<double> last_torque{0.0};

private:
    Effect* effect_slot(int index);
    void handle_packet(int ptype, const void* packet);
    double effect_force(Effect& eff, double position, double velocity, double elapsed_ms,
                        const FfbGains& gains) const;
    double direction(const Effect& eff) const;
    void log(const std::string& level, const std::string& msg) const;

    LogFn on_log_;
    mutable std::mutex lock_;
    std::map<int, Effect> effects_;
    double device_gain_ = 1.0;
    bool actuators_on_ = true;
    bool paused_ = false;
    std::string direction_mode_ = "auto";

    VJoyApi* api_ = nullptr;
    std::atomic<int> device_id_{0};
    std::atomic<bool> registered_{false};
};

} // namespace wb

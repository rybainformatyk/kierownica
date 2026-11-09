#include "processing.h"

#include <algorithm>
#include <cmath>

#include "util.h"

namespace wb {

using protocol::NUM_BUTTONS;

double pedal_position(int raw, const PedalConfig& cfg) {
    int lo = cfg.raw_min, hi = cfg.raw_max;
    double value = hi == lo ? 0.0 : static_cast<double>(raw - lo) / (hi - lo);
    value = clamp01(value);
    if (cfg.invert) value = 1.0 - value;
    return value;
}

double pedal_curve(double value, const PedalConfig& cfg) {
    // Martwa strefa: rozciagamy przedzial [dz_low, 1-dz_high] na pelne 0..1
    double dz_lo = cfg.deadzone_low;
    double dz_hi = cfg.deadzone_high;
    double span = 1.0 - dz_lo - dz_hi;
    if (span <= 1e-6)
        value = value < 0.5 ? 0.0 : 1.0;
    else
        value = clamp01((value - dz_lo) / span);

    if (cfg.gamma != 1.0) value = std::pow(value, cfg.gamma);
    return value;
}

double steer_curve(double axis, double linearity) {
    axis = clamp_sym(axis);
    if (linearity != 1.0) {
        double sign = axis < 0 ? -1.0 : 1.0;
        axis = sign * std::pow(std::abs(axis), linearity);
    }
    return axis;
}

void PedalProcessor::reset() {
    smoothed_ = 0.0;
    primed_ = false;
}

double PedalProcessor::process(int raw, const PedalConfig& cfg) {
    double value = pedal_curve(pedal_position(raw, cfg), cfg);
    double a = cfg.smoothing;
    if (a > 0.0) {
        if (!primed_) {
            smoothed_ = value;
            primed_ = true;
        } else {
            smoothed_ += (value - smoothed_) * (1.0 - a);
        }
        value = smoothed_;
    } else {
        smoothed_ = value;
        primed_ = true;
    }
    return clamp01(value);
}

void SteerProcessor::reset() {
    smoothed_deg_ = 0.0;
    primed_ = false;
    last_deg_ = 0.0;
    last_t_ = 0.0;
    velocity_ = 0.0;
}

std::pair<double, double> SteerProcessor::process(int32_t counts, const SteerConfig& cfg,
                                                  std::optional<double> now_opt) {
    double now = now_opt ? *now_opt : monotonic();

    int64_t rel = static_cast<int64_t>(counts) - cfg.center_counts;
    double deg = static_cast<double>(rel) * 360.0 / std::max(4, cfg.counts_per_rev);
    if (cfg.invert) deg = -deg;

    double a = cfg.smoothing;
    if (a > 0.0) {
        if (!primed_) {
            smoothed_deg_ = deg;
            primed_ = true;
        } else {
            smoothed_deg_ += (deg - smoothed_deg_) * (1.0 - a);
        }
        deg = smoothed_deg_;
    } else {
        smoothed_deg_ = deg;
        primed_ = true;
    }

    // predkosc katowa - potrzebna do tlumienia (damper) w FFB
    if (last_t_ != 0.0) {
        double dt = now - last_t_;
        if (dt > 1e-4) {
            double inst = (deg - last_deg_) / dt;
            // filtr EMA, zeby nie szarpalo od szumu kwantyzacji
            velocity_ += (inst - velocity_) * 0.25;
            last_deg_ = deg;
            last_t_ = now;
        }
    } else {
        last_deg_ = deg;
        last_t_ = now;
    }

    double half = std::max(1.0, cfg.range_deg / 2.0);
    double axis = steer_curve(deg / half, cfg.linearity);
    return {deg, axis};
}

void StateProcessor::reset() {
    steer.reset();
    for (auto& p : pedals) p.reset();
}

ProcessedState StateProcessor::process(const protocol::StateFrame& frame, const AppConfig& cfg,
                                       std::optional<double> now) {
    ProcessedState out;
    auto [deg, axis] = steer.process(frame.steer_counts, cfg.steer, now);
    out.steer_deg = deg;
    out.steer_axis = axis;
    out.steer_velocity = steer.velocity_deg_s();

    for (size_t i = 0; i < NUM_PEDALS; ++i) {
        const PedalConfig& pcfg = cfg.pedals[i];
        if (pcfg.source < 0 || pcfg.source >= static_cast<int>(frame.axes.size())) {
            out.pedals[i] = 0.0;
            pedals[i].reset();
            continue;
        }
        out.pedals[i] = pedals[i].process(frame.axes[static_cast<size_t>(pcfg.source)], pcfg);
    }

    // Inwersja (styk normalnie zwarty) dziala na masce, jeszcze przed
    // krzyzakiem i mapowaniem - dzieki temu odwrocony przycisk zachowuje
    // sie tak samo niezaleznie od tego, do czego zostal przypisany.
    uint32_t inv_mask = 0;
    for (size_t i = 0; i < cfg.buttons.invert.size() && i < NUM_BUTTONS; ++i)
        if (cfg.buttons.invert[i]) inv_mask |= uint32_t(1) << i;
    uint32_t effective = frame.buttons ^ inv_mask;

    int hat = -1;
    uint32_t hat_used = 0;
    const auto& hat_src = cfg.buttons.hat;
    for (size_t direction = 0; direction < hat_src.size(); ++direction) {
        int btn = hat_src[direction];
        if (btn >= 0 && btn < NUM_BUTTONS) {
            hat_used |= uint32_t(1) << btn;
            if (hat == -1 && ((effective >> btn) & 1)) hat = static_cast<int>(direction);
        }
    }

    const auto& mapping = cfg.buttons.mapping;
    for (int i = 0; i < NUM_BUTTONS; ++i) {
        if ((hat_used >> i) & 1) continue;
        if (!((effective >> i) & 1)) continue;
        int target = i < static_cast<int>(mapping.size()) ? mapping[static_cast<size_t>(i)] : 0;
        if (target > 0) out.out_buttons[static_cast<size_t>(out.out_buttons_count++)] = static_cast<uint8_t>(target);
    }

    out.raw_axes = frame.axes;
    out.raw_counts = frame.steer_counts;
    out.buttons_raw = frame.buttons;
    out.buttons_effective = effective;
    out.hat = hat;
    out.flags = frame.flags;
    out.seq = frame.seq;
    return out;
}

// ----------------------------------------------------- tryb pada (XInput)

namespace {

// Kierunki krzyzaka w kolejnosci uzywanej przez ProcessedState.hat.
const char* const HAT_TO_DPAD[4] = {"DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT"};

// Martwa strefa wokol zera, z rozciagnieciem reszty na pelny zakres.
double apply_deadzone(double value, double dz) {
    if (dz <= 0.0) return value;
    double a = std::abs(value);
    if (a <= dz) return 0.0;
    double span = 1.0 - dz;
    if (span <= 1e-6) return value > 0 ? 1.0 : -1.0;
    double scaled = (a - dz) / span;
    return value > 0 ? scaled : -scaled;
}

// Kompensacja martwej strefy GRY - odwrotnosc apply_deadzone. Wejscie 0
// zostaje zerem (auto nie moze skrecac samo), najmniejszy ruch od razu daje
// `anti`, a wejscie 1 nadal daje 1.
double apply_antideadzone(double value, double anti) {
    if (anti <= 0.0) return value;
    double a = std::abs(value);
    if (a <= 1e-9) return 0.0;
    double scaled = anti + (1.0 - anti) * a;
    scaled = scaled > 1.0 ? 1.0 : scaled;
    return value > 0 ? scaled : -scaled;
}

} // namespace

std::map<std::string, double> pad_sources(const ProcessedState& state, const AppConfig& cfg) {
    const PadConfig& pad = cfg.pad;
    double half = std::max(1.0, pad.steer_range_deg / 2.0);
    double steer = apply_deadzone(clamp_sym(state.steer_deg / half), pad.steer_deadzone);
    // Kolejnosc ma znaczenie: najpierw krzywa (ksztaltuje srodek zakresu),
    // dopiero potem kompensacja progu gry.
    if (pad.steer_linearity != 1.0) {
        double sign = steer < 0 ? -1.0 : 1.0;
        steer = sign * std::pow(std::abs(steer), pad.steer_linearity);
    }
    steer = apply_antideadzone(steer, pad.steer_antideadzone);
    if (pad.steer_invert) steer = -steer;

    std::map<std::string, double> out;
    out["steer_left"] = std::max(0.0, -steer);
    out["steer_right"] = std::max(0.0, steer);
    for (size_t i = 0; i < NUM_PEDALS; ++i)
        out[PEDAL_KEYS[i]] = cfg.pedals[i].source < 0 ? 0.0 : clamp01(state.pedals[i]);
    return out;
}

PadState build_pad_state(const ProcessedState& state, const AppConfig& cfg) {
    const PadConfig& pad = cfg.pad;
    auto src = pad_sources(state, cfg);

    auto half = [&](const std::string& direction) {
        auto it = pad.axes.find(direction);
        if (it == pad.axes.end()) return 0.0;
        auto v = src.find(it->second);
        return v == src.end() ? 0.0 : v->second;
    };
    auto stick = [&](const std::string& axis) {
        double v = clamp_sym(half(axis + "+") - half(axis + "-"));
        return static_cast<int>(py_round(v * (v >= 0 ? 32767 : 32768)));
    };
    auto trigger = [&](const std::string& name) {
        double v = apply_antideadzone(clamp01(half(name)), pad.trigger_antideadzone);
        return static_cast<int>(py_round(clamp01(v) * 255));
    };

    unsigned buttons = 0;
    Names assigned_dpad;
    for (const auto& name : PAD_BUTTON_NAMES) {
        auto it = pad.buttons.find(name);
        int idx = it == pad.buttons.end() ? -1 : it->second;
        if (!(idx >= 0 && idx < NUM_BUTTONS)) continue;
        if (name.rfind("DPAD_", 0) == 0) assigned_dpad.push_back(name);
        if ((state.buttons_effective >> idx) & 1) buttons |= xusb_mask(name);
    }

    // Krzyzak z zakladki Przyciski dziala takze w trybie pada, ale tylko dla
    // kierunkow, ktorym nie przypisano tu wlasnego przycisku.
    if (state.hat >= 0 && state.hat < 4) {
        const char* name = HAT_TO_DPAD[state.hat];
        if (!contains(assigned_dpad, name)) buttons |= xusb_mask(name);
    }

    PadState out;
    out.buttons = buttons;
    out.lt = trigger("LT");
    out.rt = trigger("RT");
    out.lx = stick("LX");
    out.ly = stick("LY");
    out.rx = stick("RX");
    out.ry = stick("RY");
    return out;
}

// ------------------------------------------------- tryb G29 (Native USB)

namespace {

// Krzyzak G29 liczy co 45 stopni od gory zgodnie z zegarem; 8 = srodek.
struct HatDir { int dx, dy; };
constexpr HatDir G29_HAT[8] = {{0, 1}, {1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}};

int g29_hat_value(int dx, int dy) {
    for (int i = 0; i < 8; ++i)
        if (G29_HAT[i].dx == dx && G29_HAT[i].dy == dy) return i;
    return G29_HAT_CENTER;
}

uint8_t g29_pedal(double value, bool inverted) {
    int v = static_cast<int>(py_round(clamp01(value) * 255));
    return static_cast<uint8_t>(inverted ? 255 - v : v);
}

} // namespace

protocol::G29Report build_g29_report(const ProcessedState& state, const AppConfig& cfg) {
    const ButtonMap& assign = cfg.g29.buttons;
    uint32_t eff = state.buttons_effective;

    // 1/0 dla przypisanej kontrolki, -1 gdy gniazdo puste.
    auto pressed = [&](const std::string& name) {
        auto it = assign.find(name);
        int idx = it == assign.end() ? -1 : it->second;
        if (!(idx >= 0 && idx < NUM_BUTTONS)) return -1;
        return static_cast<int>((eff >> idx) & 1);
    };

    // przyciski 1..25 leza w bitach 4..28 pierwszych czterech bajtow
    uint32_t mask = 0;
    for (size_t i = 0; i < G29_BUTTON_NAMES.size(); ++i)
        if (pressed(G29_BUTTON_NAMES[i]) == 1) mask |= uint32_t(1) << (4 + i);

    // Krzyzak: wlasne przypisania G29, a kierunki bez przypisania biora
    // krzyzak z zakladki Przyciski - tak samo jak w trybie pada.
    bool dirs[4];
    for (int d = 0; d < 4; ++d) {
        int on = pressed(G29_DPAD[static_cast<size_t>(d)]);
        dirs[d] = on == -1 ? state.hat == d : on == 1;
    }
    bool up = dirs[0], right = dirs[1], down = dirs[2], left = dirs[3];
    mask |= static_cast<uint32_t>(g29_hat_value(int(right) - int(left), int(up) - int(down)));

    double axis = state.steer_axis;
    axis = std::isnan(axis) ? 0.0 : clamp_sym(axis);
    long long wheel = py_round((axis + 1.0) * 32767.5);
    wheel = wheel < 0 ? 0 : (wheel > 65535 ? 65535 : wheel);

    bool inv = cfg.g29.pedals_inverted;
    auto pedal = [&](const char* key) {
        int i = pedal_index(key);
        if (cfg.pedals[static_cast<size_t>(i)].source < 0) return g29_pedal(0.0, inv);  // wylaczony kanal = puszczony
        return g29_pedal(state.pedals[static_cast<size_t>(i)], inv);
    };

    return protocol::G29Report{
        static_cast<uint8_t>(mask & 0xFF),
        static_cast<uint8_t>((mask >> 8) & 0xFF),
        static_cast<uint8_t>((mask >> 16) & 0xFF),
        static_cast<uint8_t>((mask >> 24) & 0xFF),
        static_cast<uint8_t>(wheel & 0xFF),
        static_cast<uint8_t>(wheel >> 8),
        pedal("throttle"),  // Z  - gaz
        pedal("brake"),     // Rz - hamulec
        pedal("clutch"),    // Y  - sprzeglo
        0x80, 0x80,         // drazek skrzyni w spoczynku
        0x00,
    };
}

std::map<std::string, bool> g29_active(const protocol::G29Report& report) {
    std::map<std::string, bool> out;
    uint32_t mask = report[0] | (uint32_t(report[1]) << 8) | (uint32_t(report[2]) << 16) |
                    (uint32_t(report[3]) << 24);
    for (size_t i = 0; i < G29_BUTTON_NAMES.size(); ++i)
        out[G29_BUTTON_NAMES[i]] = (mask & (uint32_t(1) << (4 + i))) != 0;
    int dx = 0, dy = 0;
    unsigned hat = mask & 0x0F;
    if (hat < 8) {
        dx = G29_HAT[hat].dx;
        dy = G29_HAT[hat].dy;
    }
    out["DPAD_UP"] = dy > 0;
    out["DPAD_DOWN"] = dy < 0;
    out["DPAD_RIGHT"] = dx > 0;
    out["DPAD_LEFT"] = dx < 0;
    return out;
}

} // namespace wb

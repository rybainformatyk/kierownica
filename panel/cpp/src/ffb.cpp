#include "ffb.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "outputs/vjoy.h"
#include "util.h"

namespace wb {

namespace {

constexpr double PID_SCALE = 10000.0;  // pelna skala wartosci HID PID
constexpr long long PID_LIMIT = 10001; // wartosci poza tym zakresem to blad parsowania

// Uklad pol odpowiada public.h z SDK vJoy. Wyrownanie naturalne.
struct FFB_EFF_REPORT {
    BYTE EffectBlockIndex;
    int EffectType;
    WORD Duration;
    WORD TrigerRpt;
    WORD SamplePrd;
    BYTE Gain;
    BYTE TrigerBtn;
    int Polar;
    BYTE Direction;
    BYTE DirY;
};
struct FFB_EFF_OP {
    BYTE EffectBlockIndex;
    int EffectOp;
    BYTE LoopCount;
};
struct FFB_EFF_CONSTANT {
    BYTE EffectBlockIndex;
    LONG Magnitude;
};
struct FFB_EFF_RAMP {
    BYTE EffectBlockIndex;
    LONG Start;
    LONG End;
};
struct FFB_EFF_PERIOD {
    BYTE EffectBlockIndex;
    DWORD Magnitude;
    LONG Offset;
    DWORD Phase;
    DWORD Period;
};
struct FFB_EFF_COND {
    BYTE EffectBlockIndex;
    int isY;
    LONG CenterPointOffset;
    LONG PosCoeff;
    LONG NegCoeff;
    DWORD PosSatur;
    DWORD NegSatur;
    LONG DeadBand;
};
struct FFB_EFF_ENVLP {
    BYTE EffectBlockIndex;
    DWORD AttackLevel;
    DWORD FadeLevel;
    DWORD AttackTime;
    DWORD FadeTime;
};

std::optional<double> norm(long long value) {
    if (value > PID_LIMIT || value < -PID_LIMIT) return std::nullopt;
    return std::max(-1.0, std::min(1.0, static_cast<double>(value) / PID_SCALE));
}

std::optional<double> norm_u(long long value) {
    if (value < 0 || value > PID_LIMIT) return std::nullopt;
    return std::max(0.0, std::min(1.0, static_cast<double>(value) / PID_SCALE));
}

double periodic(const Effect& eff, double elapsed_ms) {
    int period = std::max(1, eff.period_ms);
    double phase = elapsed_ms / period + eff.phase_deg / 360.0;
    phase -= std::floor(phase);
    double w;
    switch (eff.type) {
    case ET_SINE: w = std::sin(2.0 * std::numbers::pi * phase); break;
    case ET_SQR: w = phase < 0.5 ? 1.0 : -1.0; break;
    case ET_TRNGL: w = 4.0 * std::abs(phase - 0.5) - 1.0; break;
    case ET_STUP: w = 2.0 * phase - 1.0; break;
    default: w = 1.0 - 2.0 * phase; break;  // ET_STDN
    }
    return std::max(-1.0, std::min(1.0, eff.offset + eff.amplitude * w));
}

// Wspolny wzor dla sprezyny / tlumienia / tarcia (HID PID).
double conditional(const Effect& eff, double metric) {
    double delta = metric - eff.center;
    if (std::abs(delta) <= eff.deadband) return 0.0;
    delta = delta > 0 ? delta - eff.deadband : delta + eff.deadband;
    double value, limit;
    if (delta > 0) {
        value = -delta * eff.pos_coeff;
        limit = eff.pos_sat;
    } else {
        value = -delta * eff.neg_coeff;
        limit = eff.neg_sat;
    }
    return std::max(-limit, std::min(limit, value));
}

double envelope(const Effect& eff, double elapsed_ms) {
    if (eff.attack_ms <= 0 && eff.fade_ms <= 0) return 1.0;
    double level = 1.0;
    if (eff.attack_ms > 0 && elapsed_ms < eff.attack_ms) {
        double k = elapsed_ms / eff.attack_ms;
        level = eff.attack_level + (1.0 - eff.attack_level) * k;
    }
    if (eff.duration_ms > 0 && eff.fade_ms > 0) {
        double fade_start = eff.duration_ms - eff.fade_ms;
        if (elapsed_ms > fade_start) {
            double k = std::min(1.0, (elapsed_ms - fade_start) / eff.fade_ms);
            level = std::min(level, 1.0 + (eff.fade_level - 1.0) * k);
        }
    }
    return std::max(0.0, std::min(1.0, level));
}

void WINAPI ffb_callback(void* data, void* user) noexcept {
    // Uruchamiane przez sterownik vJoy. Nie moze nic rzucic ani blokowac.
    auto* self = static_cast<FfbManager*>(user);
    if (!data || !self) return;
    try {
        self->on_packet(data);
    } catch (...) {
        self->parse_errors.fetch_add(1);
    }
}

} // namespace

const std::map<std::string, std::string> DIRECTION_LABELS = {
    {"auto", "Auto (zalecane)"},
    {"ignore", "Ignoruj kierunek (znak z magnitudy)"},
    {"cos", "Polarny - cosinus"},
    {"sin", "Polarny - sinus"}};

const char* effect_name(int type) {
    switch (type) {
    case ET_NONE: return "-";
    case ET_CONST: return "Stała siła";
    case ET_RAMP: return "Narastanie";
    case ET_SQR: return "Prostokąt";
    case ET_SINE: return "Sinus";
    case ET_TRNGL: return "Trójkąt";
    case ET_STUP: return "Piła w górę";
    case ET_STDN: return "Piła w dół";
    case ET_SPRNG: return "Sprężyna";
    case ET_DMPR: return "Tłumienie";
    case ET_INRT: return "Bezwładność";
    case ET_FRCTN: return "Tarcie";
    case ET_CSTM: return "Własny";
    default: return "?";
    }
}

FfbManager::FfbManager(LogFn on_log) : on_log_(std::move(on_log)) {}

bool FfbManager::register_callback(VJoyApi* api, int device_id) {
    if (registered_.load()) {
        // Callback jest jeden na caly proces. Po zmianie numeru urzadzenia
        // vJoy wystarczy przepuszczac pakiety nowego numeru.
        device_id_.store(device_id);
        return true;
    }
    if (!api || !api->FfbRegisterGenCB) {
        log("warn", "Ta wersja vJoyInterface.dll nie udostępnia API FFB");
        return false;
    }
    api_ = api;
    device_id_.store(device_id);
    api->FfbRegisterGenCB(&ffb_callback, this);
    registered_.store(true);
    log("ok", "Odbiór Force Feedback z gier aktywny");
    return true;
}

void FfbManager::unregister() {
    {
        std::lock_guard lock(lock_);
        effects_.clear();
    }
    registered_.store(false);
}

void FfbManager::on_packet(const void* packet) {
    if (!registered_.load() || !api_) return;
    packets.fetch_add(1);
    last_packet_time.store(monotonic());

    const VJoyApi& a = *api_;
    if (a.Ffb_h_DeviceID) {
        int dev = 0;
        if (a.Ffb_h_DeviceID(packet, &dev) == ERROR_SUCCESS && dev != device_id_.load()) return;
    }
    if (!a.Ffb_h_Type) return;
    int ptype = 0;
    if (a.Ffb_h_Type(packet, &ptype) != ERROR_SUCCESS) return;
    handle_packet(ptype, packet);
}

Effect* FfbManager::effect_slot(int index) {
    if (!(index >= 0 && index <= MAX_EFFECTS)) return nullptr;
    auto it = effects_.find(index);
    if (it != effects_.end()) return &it->second;
    if (static_cast<int>(effects_.size()) >= MAX_EFFECTS) return nullptr;
    Effect e;
    e.index = index;
    return &effects_.emplace(index, e).first->second;
}

void FfbManager::store_effect_for_test(const Effect& effect) {
    std::lock_guard lock(lock_);
    effects_[effect.index] = effect;
}

void FfbManager::handle_packet(int ptype, const void* packet) {
    const VJoyApi& a = *api_;

    // Dekoduje strukture funkcja z DLL (moze jej nie byc w starszych wersjach).
    auto fetch = [&packet](auto fn, auto& out) {
        return fn && reinterpret_cast<DWORD(__cdecl*)(const void*, void*)>(fn)(packet, &out) == ERROR_SUCCESS;
    };

    switch (ptype) {
    case PT_EFFREP: {
        FFB_EFF_REPORT rep{};
        if (!fetch(a.Ffb_h_Eff_Report, rep)) return;
        std::lock_guard lock(lock_);
        Effect* eff = effect_slot(rep.EffectBlockIndex);
        if (!eff) return;
        if (rep.EffectType >= ET_NONE && rep.EffectType <= ET_CSTM) eff->type = rep.EffectType;
        eff->duration_ms = rep.Duration;
        eff->gain = std::max(0.0, std::min(1.0, rep.Gain / 255.0));
        eff->direction = rep.Direction;
        eff->polar = rep.Polar != 0;
        return;
    }
    case PT_CONSTREP: {
        FFB_EFF_CONSTANT rep{};
        if (!fetch(a.Ffb_h_Eff_Constant, rep)) return;
        auto mag = norm(rep.Magnitude);
        if (!mag) { parse_errors.fetch_add(1); return; }
        std::lock_guard lock(lock_);
        if (Effect* eff = effect_slot(rep.EffectBlockIndex)) {
            eff->magnitude = *mag;
            if (eff->type == ET_NONE) eff->type = ET_CONST;
        }
        return;
    }
    case PT_RAMPREP: {
        FFB_EFF_RAMP rep{};
        if (!fetch(a.Ffb_h_Eff_Ramp, rep)) return;
        auto s = norm(rep.Start), e = norm(rep.End);
        if (!s || !e) { parse_errors.fetch_add(1); return; }
        std::lock_guard lock(lock_);
        if (Effect* eff = effect_slot(rep.EffectBlockIndex)) {
            eff->ramp_start = *s;
            eff->ramp_end = *e;
            if (eff->type == ET_NONE) eff->type = ET_RAMP;
        }
        return;
    }
    case PT_PRIDREP: {
        FFB_EFF_PERIOD rep{};
        if (!fetch(a.Ffb_h_Eff_Period, rep)) return;
        auto amp = norm_u(rep.Magnitude);
        auto off = norm(rep.Offset);
        if (!amp || !off) { parse_errors.fetch_add(1); return; }
        std::lock_guard lock(lock_);
        if (Effect* eff = effect_slot(rep.EffectBlockIndex)) {
            eff->amplitude = *amp;
            eff->offset = *off;
            eff->phase_deg = (rep.Phase % 36000) / 100.0;
            eff->period_ms = static_cast<int>(std::max<DWORD>(1, std::min<DWORD>(32767, rep.Period)));
        }
        return;
    }
    case PT_CONDREP: {
        FFB_EFF_COND rep{};
        if (!fetch(a.Ffb_h_Eff_Cond, rep)) return;
        if (rep.isY) return;  // os Y nas nie dotyczy
        auto center = norm(rep.CenterPointOffset);
        auto pos_c = norm(rep.PosCoeff);
        auto neg_c = norm(rep.NegCoeff);
        auto pos_s = norm_u(rep.PosSatur);
        auto neg_s = norm_u(rep.NegSatur);
        auto dead = norm_u(rep.DeadBand);
        if (!center || !pos_c || !neg_c || !pos_s || !neg_s || !dead) { parse_errors.fetch_add(1); return; }
        std::lock_guard lock(lock_);
        if (Effect* eff = effect_slot(rep.EffectBlockIndex)) {
            eff->center = *center;
            eff->pos_coeff = *pos_c;
            eff->neg_coeff = *neg_c;
            eff->pos_sat = *pos_s;
            eff->neg_sat = *neg_s;
            eff->deadband = *dead;
        }
        return;
    }
    case PT_ENVREP: {
        FFB_EFF_ENVLP rep{};
        if (!fetch(a.Ffb_h_Eff_Envlp, rep)) return;
        auto al = norm_u(rep.AttackLevel), fl = norm_u(rep.FadeLevel);
        if (!al || !fl) { parse_errors.fetch_add(1); return; }
        std::lock_guard lock(lock_);
        if (Effect* eff = effect_slot(rep.EffectBlockIndex)) {
            eff->attack_level = *al;
            eff->fade_level = *fl;
            eff->attack_ms = static_cast<int>(std::min<DWORD>(60000, rep.AttackTime));
            eff->fade_ms = static_cast<int>(std::min<DWORD>(60000, rep.FadeTime));
        }
        return;
    }
    case PT_EFOPREP: {
        FFB_EFF_OP rep{};
        if (!fetch(a.Ffb_h_EffOp, rep)) return;
        std::lock_guard lock(lock_);
        Effect* eff = effect_slot(rep.EffectBlockIndex);
        if (!eff) return;
        if (rep.EffectOp == EFF_START || rep.EffectOp == EFF_SOLO) {
            if (rep.EffectOp == EFF_SOLO)
                for (auto& [idx, other] : effects_)
                    if (&other != eff) other.reset_state();
            eff->active = true;
            eff->start_time = monotonic();
            eff->loop_count = std::max(1, rep.LoopCount ? static_cast<int>(rep.LoopCount) : 1);
        } else if (rep.EffectOp == EFF_STOP) {
            eff->reset_state();
        }
        return;
    }
    case PT_BLKFRREP: {
        int index = 0;
        bool ok = fetch(a.Ffb_h_EBI, index);
        std::lock_guard lock(lock_);
        if (!ok) effects_.clear();
        else effects_.erase(index);
        return;
    }
    case PT_CTRLREP: {
        int ctrl = 0;
        if (!fetch(a.Ffb_h_DevCtrl, ctrl)) return;
        std::lock_guard lock(lock_);
        switch (ctrl) {
        case CTRL_ENACT: actuators_on_ = true; break;
        case CTRL_DISACT: actuators_on_ = false; break;
        case CTRL_STOPALL:
            for (auto& [idx, eff] : effects_) eff.reset_state();
            break;
        case CTRL_DEVRST:
            effects_.clear();
            device_gain_ = 1.0;
            actuators_on_ = true;
            paused_ = false;
            break;
        case CTRL_DEVPAUSE: paused_ = true; break;
        case CTRL_DEVCONT: paused_ = false; break;
        default: break;
        }
        return;
    }
    case PT_GAINREP: {
        BYTE gain = 255;
        if (!fetch(a.Ffb_h_DevGain, gain)) return;
        std::lock_guard lock(lock_);
        device_gain_ = std::max(0.0, std::min(1.0, gain / 255.0));
        return;
    }
    default:
        return;
    }
}

void FfbManager::reset() {
    {
        std::lock_guard lock(lock_);
        effects_.clear();
        device_gain_ = 1.0;
        actuators_on_ = true;
        paused_ = false;
    }
    last_torque.store(0.0);
}

std::vector<std::tuple<int, std::string, double>> FfbManager::active_effects() const {
    std::vector<std::tuple<int, std::string, double>> out;
    std::lock_guard lock(lock_);
    for (const auto& [idx, e] : effects_)
        if (e.active) out.emplace_back(e.index, effect_name(e.type), e.magnitude);
    return out;
}

double FfbManager::device_gain() const {
    std::lock_guard lock(lock_);
    return device_gain_;
}

void FfbManager::set_direction_mode(const std::string& mode) {
    std::lock_guard lock(lock_);
    direction_mode_ = mode;
}

double FfbManager::compute(double position, double velocity_norm, const FfbGains& gains,
                           std::optional<double> now_opt) {
    double now = now_opt ? *now_opt : monotonic();
    double total = 0.0;
    {
        std::lock_guard lock(lock_);
        if (paused_ || !actuators_on_) {
            last_torque.store(0.0);
            return 0.0;
        }
        for (auto& [idx, eff] : effects_) {
            if (!eff.active) continue;
            double elapsed_ms = (now - eff.start_time) * 1000.0;
            if (eff.duration_ms && elapsed_ms > static_cast<double>(eff.duration_ms) * eff.loop_count) {
                eff.reset_state();
                continue;
            }
            total += effect_force(eff, position, velocity_norm, elapsed_ms, gains);
        }
        total *= device_gain_;
    }
    total = std::max(-1.0, std::min(1.0, total));
    last_torque.store(total);
    return total;
}

double FfbManager::effect_force(Effect& eff, double position, double velocity, double elapsed_ms,
                                const FfbGains& gains) const {
    double force = 0.0;
    double category = gains.constant;
    switch (eff.type) {
    case ET_CONST:
        force = eff.magnitude * direction(eff);
        category = gains.constant;
        break;
    case ET_RAMP: {
        double k = eff.duration_ms > 0 ? std::max(0.0, std::min(1.0, elapsed_ms / eff.duration_ms)) : 0.0;
        force = (eff.ramp_start + (eff.ramp_end - eff.ramp_start) * k) * direction(eff);
        category = gains.constant;
        break;
    }
    case ET_SINE: case ET_SQR: case ET_TRNGL: case ET_STUP: case ET_STDN:
        force = periodic(eff, elapsed_ms) * direction(eff);
        category = gains.periodic;
        break;
    case ET_SPRNG:
        force = conditional(eff, position);
        category = gains.spring;
        break;
    case ET_DMPR:
        force = conditional(eff, velocity);
        category = gains.damper;
        break;
    case ET_FRCTN: {
        double sign = std::abs(velocity) < 1e-3 ? 0.0 : (velocity > 0 ? 1.0 : -1.0);
        force = conditional(eff, sign);
        category = gains.damper;
        break;
    }
    case ET_INRT:
        // bezwladnosc wymaga przyspieszenia - pomijamy swiadomie,
        // zamiast zglaszac blad
        force = 0.0;
        category = gains.damper;
        break;
    default:
        break;
    }
    return force * eff.gain * category * envelope(eff, elapsed_ms);
}

double FfbManager::direction(const Effect& eff) const {
    if (direction_mode_ == "ignore") return 1.0;
    double angle = eff.direction * 2.0 * std::numbers::pi / 255.0;
    if (direction_mode_ == "cos") return std::cos(angle);
    if (direction_mode_ == "sin") return std::sin(angle);
    // auto: kierunek bierzemy z cosinusa, ale gdy wypada bliski zera
    // (gra uzywa znaku samej magnitudy) zostawiamy +1
    double c = std::cos(angle);
    if (std::abs(c) < 0.05) {
        double s = std::sin(angle);
        return std::abs(s) < 0.05 ? 1.0 : (s > 0 ? 1.0 : -1.0);
    }
    return c > 0 ? 1.0 : -1.0;
}

void FfbManager::log(const std::string& level, const std::string& msg) const {
    if (!on_log_) return;
    try {
        on_log_(level, msg);
    } catch (...) {
    }
}

} // namespace wb

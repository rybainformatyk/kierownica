#include "config.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "json.h"
#include "util.h"

namespace fs = std::filesystem;
using wb::json::Value;

namespace wb {

const std::array<std::string, NUM_PEDALS> PEDAL_KEYS = {"throttle", "brake", "clutch",
                                                        "aux1",     "aux2",  "aux3"};
const LabelMap PEDAL_LABELS = {{"throttle", "Gaz"},   {"brake", "Hamulec"}, {"clutch", "Sprzęgło"},
                               {"aux1", "AUX 1"},     {"aux2", "AUX 2"},    {"aux3", "AUX 3"}};

// Domyslne przypisanie osi wyjsciowych wg ukladu Logitech G29.
const LabelMap DEFAULT_VJOY_AXIS = {{"steer", "X"}, {"throttle", "Y"}, {"brake", "Z"},  {"clutch", "RZ"},
                                    {"aux1", "RX"}, {"aux2", "RY"},    {"aux3", "SL0"}};
const Names VJOY_AXIS_CHOICES = {"X", "Y", "Z", "RX", "RY", "RZ", "SL0", "SL1", "WHL", "-"};

const Names PAD_BUTTON_NAMES = {"A",  "B",  "X",    "Y",     "LB",    "RB",      "LS",        "RS",
                                "BACK", "START", "GUIDE", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"};

// GUIDE celowo zostaje pusty (otwiera nakladke Xboxa).
const std::map<std::string, int> DEFAULT_PAD_BUTTONS = {
    {"A", 0},      {"B", 1},       {"X", 2},       {"Y", 3},         {"LB", 4},
    {"RB", 5},     {"LS", 8},      {"RS", 9},      {"BACK", 6},      {"START", 7},
    {"GUIDE", -1}, {"DPAD_UP", 10}, {"DPAD_DOWN", 11}, {"DPAD_LEFT", 12}, {"DPAD_RIGHT", 13}};

const Names PAD_DIRECTIONS = {"LX-", "LX+", "LY+", "LY-", "RX-", "RX+", "RY+", "RY-", "LT", "RT"};

const LabelMap PAD_DIRECTION_LABELS = {
    {"LX-", "Lewy drążek w lewo"},   {"LX+", "Lewy drążek w prawo"},
    {"LY+", "Lewy drążek w górę"},   {"LY-", "Lewy drążek w dół"},
    {"RX-", "Prawy drążek w lewo"},  {"RX+", "Prawy drążek w prawo"},
    {"RY+", "Prawy drążek w górę"},  {"RY-", "Prawy drążek w dół"},
    {"LT", "Lewy spust (LT)"},       {"RT", "Prawy spust (RT)"}};

const LabelMap PAD_DIRECTION_SHORT = {
    {"LX-", "w lewo"}, {"LX+", "w prawo"}, {"LY+", "w górę"}, {"LY-", "w dół"},
    {"RX-", "w lewo"}, {"RX+", "w prawo"}, {"RY+", "w górę"}, {"RY-", "w dół"},
    {"LT", "LT - lewy spust"}, {"RT", "RT - prawy spust"}};

const Names PAD_SOURCE_KEYS = {"steer_left", "steer_right", "throttle", "brake",
                               "clutch",     "aux1",        "aux2",     "aux3"};
const LabelMap PAD_SOURCE_LABELS = {
    {"steer_left", "Kierownica w lewo"}, {"steer_right", "Kierownica w prawo"},
    {"throttle", "Gaz"}, {"brake", "Hamulec"}, {"clutch", "Sprzęgło"},
    {"aux1", "AUX 1"}, {"aux2", "AUX 2"}, {"aux3", "AUX 3"}};

// Domyslnie: skret na lewym drazku, gaz na prawym spuscie, hamulec na lewym.
const LabelMap DEFAULT_PAD_AXES = {
    {"LX-", "steer_left"}, {"LX+", "steer_right"}, {"RT", "throttle"}, {"LT", "brake"}};

namespace {

// Stary uklad konfiguracji (jedno zrodlo -> cala os) - tylko do migracji.
const Names LEGACY_PAD_TARGETS = {"-", "LX", "LY", "RX", "RY", "LT", "RT"};

struct PadProfile {
    int steer_range_deg;
    double steer_deadzone;
    double steer_antideadzone;
    double steer_linearity;
    double trigger_antideadzone;
};

// Gotowe nastawy trybu pada pod konkretne gry. Mapowanie przyciskow i
// kierunkow NIE jest czescia profilu. Forza ma wlasna martwa strefe drazka -
// kompensacja sprawia, ze najmniejszy ruch od razu przebija ten prog.
const std::map<std::string, PadProfile, std::less<>> PAD_PROFILES = {
    {"nfs", {360, 0.0, 0.0, 1.0, 0.0}},
    {"forza", {450, 0.0, 0.15, 1.0, 0.0}},
};

constexpr int SAVE_ATTEMPTS = 10;  // ile razy probujemy, gdy plik jest chwilowo zajety

double clamp_nan(double v, double lo, double hi) {
    if (std::isnan(v)) return lo;
    return std::max(lo, std::min(hi, v));
}

int clamp_int(int64_t v, int lo, int hi) {
    return static_cast<int>(std::max<int64_t>(lo, std::min<int64_t>(hi, v)));
}

} // namespace

const Names PAD_PROFILE_ORDER = {"", "nfs", "forza"};
const LabelMap PAD_PROFILE_LABELS = {
    {"", "Własny"}, {"nfs", "NFS i gry zręcznościowe"}, {"forza", "Forza Horizon / Motorsport"}};

const Names OUTPUT_MODES = {"g29", "g29_vjoy", "wheel", "pad", "both", "none"};
const LabelMap OUTPUT_MODE_LABELS = {
    {"g29", "Kierownica G29 (Native USB) - każda gra"},
    {"g29_vjoy", "G29 + vJoy - jazda na G29, przyciski z vJoy"},
    {"wheel", "Kierownica (vJoy) - symulatory, Force Feedback"},
    {"pad", "Pad Xbox 360 (ViGEm) - gry XInput, np. NFS"},
    {"both", "Oba naraz - kierownica i pad"},
    {"none", "Tylko podgląd - nic nie wysyłaj"}};

const Names G29_BUTTON_NAMES = {"CROSS",   "SQUARE",  "CIRCLE",   "TRIANGLE", "PADDLE_R",
                                "PADDLE_L", "R2",     "L2",       "SHARE",    "OPTIONS",
                                "R3",      "L3",      "GEAR_1",   "GEAR_2",   "GEAR_3",
                                "GEAR_4",  "GEAR_5",  "GEAR_6",   "GEAR_R",   "PLUS",
                                "MINUS",   "DIAL_CW", "DIAL_CCW", "ENTER",    "PS"};
const Names G29_DPAD = {"DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT"};

static Names concat(const Names& a, const Names& b) {
    Names out = a;
    out.insert(out.end(), b.begin(), b.end());
    return out;
}
const Names G29_CONTROLS = concat(G29_BUTTON_NAMES, G29_DPAD);

const LabelMap G29_CONTROL_LABELS = {
    {"CROSS", "Krzyżyk"}, {"SQUARE", "Kwadrat"}, {"CIRCLE", "Kółko"}, {"TRIANGLE", "Trójkąt"},
    {"PADDLE_R", "Prawa łopatka"}, {"PADDLE_L", "Lewa łopatka"},
    {"R2", "R2"}, {"L2", "L2"}, {"R3", "R3"}, {"L3", "L3"},
    {"SHARE", "Share"}, {"OPTIONS", "Options"}, {"PS", "PS"},
    {"GEAR_1", "Bieg 1"}, {"GEAR_2", "Bieg 2"}, {"GEAR_3", "Bieg 3"},
    {"GEAR_4", "Bieg 4"}, {"GEAR_5", "Bieg 5"}, {"GEAR_6", "Bieg 6"}, {"GEAR_R", "Wsteczny"},
    {"PLUS", "Plus (+)"}, {"MINUS", "Minus (-)"},
    {"DIAL_CW", "Pokrętło w prawo"}, {"DIAL_CCW", "Pokrętło w lewo"},
    {"ENTER", "Enter (pokrętło)"},
    {"DPAD_UP", "Krzyżak w górę"}, {"DPAD_RIGHT", "Krzyżak w prawo"},
    {"DPAD_DOWN", "Krzyżak w dół"}, {"DPAD_LEFT", "Krzyżak w lewo"}};

const std::vector<std::pair<std::string, std::string>> PAD_TO_G29 = {
    {"A", "CROSS"}, {"B", "CIRCLE"}, {"X", "SQUARE"}, {"Y", "TRIANGLE"},
    {"LB", "PADDLE_L"}, {"RB", "PADDLE_R"}, {"LS", "L3"}, {"RS", "R3"},
    {"BACK", "SHARE"}, {"START", "OPTIONS"}, {"GUIDE", "PS"},
    {"DPAD_UP", "DPAD_UP"}, {"DPAD_DOWN", "DPAD_DOWN"},
    {"DPAD_LEFT", "DPAD_LEFT"}, {"DPAD_RIGHT", "DPAD_RIGHT"}};

const Names DIRECTION_MODES = {"auto", "ignore", "cos", "sin"};

std::string last_load_problem;

int pedal_index(std::string_view key) {
    for (int i = 0; i < NUM_PEDALS; ++i)
        if (PEDAL_KEYS[static_cast<size_t>(i)] == key) return i;
    return -1;
}

bool contains(const Names& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

ButtonMap g29_buttons_from_pad(const ButtonMap& pad_buttons) {
    ButtonMap out;
    for (const auto& name : G29_CONTROLS) out[name] = -1;
    for (const auto& [pad_name, g29_name] : PAD_TO_G29) {
        auto it = pad_buttons.find(pad_name);
        int idx = it == pad_buttons.end() ? -1 : it->second;
        out[g29_name] = (idx >= 0 && idx < protocol::NUM_BUTTONS) ? idx : -1;
    }
    return out;
}

ButtonsConfig::ButtonsConfig() : hat(4, -1), invert(protocol::NUM_BUTTONS, false) {
    for (int i = 1; i <= protocol::NUM_BUTTONS; ++i) mapping.push_back(i);
}

PadConfig::PadConfig() : axes(DEFAULT_PAD_AXES), buttons(DEFAULT_PAD_BUTTONS) {}

G29Config::G29Config() : buttons(g29_buttons_from_pad(DEFAULT_PAD_BUTTONS)) {}

bool PadConfig::apply_profile(std::string_view key) {
    auto it = PAD_PROFILES.find(key);
    if (it == PAD_PROFILES.end()) {
        profile.clear();
        return false;
    }
    const PadProfile& v = it->second;
    steer_range_deg = v.steer_range_deg;
    steer_deadzone = v.steer_deadzone;
    steer_antideadzone = v.steer_antideadzone;
    steer_linearity = v.steer_linearity;
    trigger_antideadzone = v.trigger_antideadzone;
    profile = std::string(key);
    return true;
}

bool PadConfig::matches_profile(std::string_view key) const {
    auto it = PAD_PROFILES.find(key);
    if (it == PAD_PROFILES.end()) return false;
    const PadProfile& v = it->second;
    auto eq = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    return eq(steer_range_deg, v.steer_range_deg) && eq(steer_deadzone, v.steer_deadzone) &&
           eq(steer_antideadzone, v.steer_antideadzone) && eq(steer_linearity, v.steer_linearity) &&
           eq(trigger_antideadzone, v.trigger_antideadzone);
}

std::string PadConfig::detect_profile() const {
    for (const auto& key : PAD_PROFILE_ORDER)
        if (!key.empty() && matches_profile(key)) return key;
    return {};
}

std::string PadConfig::source_of(const std::string& direction) const {
    auto it = axes.find(direction);
    return it == axes.end() ? std::string() : it->second;
}

Names PadConfig::directions_of(const std::string& source) const {
    Names out;
    for (const auto& d : PAD_DIRECTIONS)
        if (source_of(d) == source) out.push_back(d);
    return out;
}

std::optional<bool> OutputConfig::vjoy_wanted() const {
    if (wheel_on()) return true;
    if (!hide_unused || mode == "none") return std::nullopt;
    return false;
}

static fs::path g_config_override;

void set_config_path_override(const fs::path& path) { g_config_override = path; }

fs::path config_path() {
    if (!g_config_override.empty()) return g_config_override;
    // Obok programu, jesli da sie pisac, inaczej w %APPDATA%.
    fs::path dir = exe_dir();
    fs::path probe = dir / ".write_test";
    {
        std::ofstream f(probe, std::ios::binary);
        if (f && (f << "x") && f.flush()) {
            f.close();
            std::error_code ec;
            fs::remove(probe, ec);
            return dir / "config.json";
        }
    }
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    fs::path base;
    if (n > 0 && n < MAX_PATH) {
        base = buf;
    } else {
        n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
        base = (n > 0 && n < MAX_PATH) ? fs::path(buf) : dir;
    }
    fs::path p = base / APP_NAME;
    std::error_code ec;
    fs::create_directories(p, ec);
    return p / "config.json";
}

AppConfig::AppConfig() {
    for (int i = 0; i < NUM_PEDALS; ++i) {
        const std::string& key = PEDAL_KEYS[static_cast<size_t>(i)];
        PedalConfig& p = pedals[static_cast<size_t>(i)];
        p.source = i;
        auto it = DEFAULT_VJOY_AXIS.find(key);
        p.vjoy_axis = it == DEFAULT_VJOY_AXIS.end() ? "-" : it->second;
    }
    normalize();
}

// --------------------------------------------------------------- walidacja

void AppConfig::normalize() {
    SteerConfig& s = steer;
    s.counts_per_rev = clamp_int(s.counts_per_rev, 4, 2'000'000);
    s.range_deg = clamp_int(s.range_deg, 45, 1800);
    s.smoothing = clamp_nan(s.smoothing, 0.0, 0.95);
    s.linearity = clamp_nan(s.linearity, 0.3, 3.0);
    s.endstop_gain = clamp_int(s.endstop_gain, 1, 65535);
    if (!contains(VJOY_AXIS_CHOICES, s.vjoy_axis)) s.vjoy_axis = "X";

    for (int i = 0; i < NUM_PEDALS; ++i) {
        PedalConfig& p = pedals[static_cast<size_t>(i)];
        if (!(p.source >= -1 && p.source < protocol::NUM_AXES)) p.source = -1;
        p.raw_min = clamp_int(p.raw_min, 0, 4095);
        p.raw_max = clamp_int(p.raw_max, 0, 4095);
        if (p.raw_min > p.raw_max) {
            // odwrocony zakres zapisujemy jako normalny + flaga inwersji,
            // zeby dalej w kodzie min < max bylo zawsze prawda
            std::swap(p.raw_min, p.raw_max);
            p.invert = !p.invert;
        }
        if (p.raw_max - p.raw_min < 32) {  // bezsensowna kalibracja
            p.raw_min = 0;
            p.raw_max = 4095;
        }
        p.deadzone_low = clamp_nan(p.deadzone_low, 0.0, 0.45);
        p.deadzone_high = clamp_nan(p.deadzone_high, 0.0, 0.45);
        p.gamma = clamp_nan(p.gamma, 0.3, 3.0);
        p.smoothing = clamp_nan(p.smoothing, 0.0, 0.95);
        if (!contains(VJOY_AXIS_CHOICES, p.vjoy_axis)) {
            auto it = DEFAULT_VJOY_AXIS.find(PEDAL_KEYS[static_cast<size_t>(i)]);
            p.vjoy_axis = it == DEFAULT_VJOY_AXIS.end() ? "-" : it->second;
        }
    }

    FfbConfig& f = ffb;
    f.overall_gain = clamp_nan(f.overall_gain, 0.0, 1.0);
    f.min_force = clamp_nan(f.min_force, 0.0, 0.5);
    f.max_force = clamp_nan(f.max_force, 0.05, 1.0);
    f.deadband = clamp_nan(f.deadband, 0.0, 0.2);
    f.slew_per_ms = clamp_int(f.slew_per_ms, 256, 65280);
    f.timeout_ms = clamp_int(f.timeout_ms, 20, 2000);
    f.min_duty = clamp_int(f.min_duty, 0, 254);
    f.max_duty = clamp_int(f.max_duty, f.min_duty + 1, 255);
    f.update_hz = clamp_int(f.update_hz, 50, 1000);
    f.spring_gain = clamp_nan(f.spring_gain, 0.0, 1.0);
    f.damper_gain = clamp_nan(f.damper_gain, 0.0, 1.0);
    f.friction_gain = clamp_nan(f.friction_gain, 0.0, 1.0);
    f.game_constant_gain = clamp_nan(f.game_constant_gain, 0.0, 2.0);
    f.game_spring_gain = clamp_nan(f.game_spring_gain, 0.0, 2.0);
    f.game_damper_gain = clamp_nan(f.game_damper_gain, 0.0, 2.0);
    f.game_periodic_gain = clamp_nan(f.game_periodic_gain, 0.0, 2.0);
    if (!contains(DIRECTION_MODES, f.direction_mode)) f.direction_mode = "auto";

    ButtonsConfig& b = buttons;
    b.mapping.resize(std::min<size_t>(b.mapping.size(), protocol::NUM_BUTTONS));
    while (b.mapping.size() < protocol::NUM_BUTTONS) b.mapping.push_back(static_cast<int>(b.mapping.size()) + 1);
    for (int& v : b.mapping) v = clamp_int(v, 0, 128);
    b.hat.resize(4, -1);
    for (int& v : b.hat)
        if (!(v >= -1 && v < protocol::NUM_BUTTONS)) v = -1;
    b.invert.resize(protocol::NUM_BUTTONS, false);

    PadConfig& d = pad;
    d.steer_range_deg = clamp_int(d.steer_range_deg, 45, 1800);
    d.steer_deadzone = clamp_nan(d.steer_deadzone, 0.0, 0.45);
    d.steer_antideadzone = clamp_nan(d.steer_antideadzone, 0.0, 0.60);
    d.trigger_antideadzone = clamp_nan(d.trigger_antideadzone, 0.0, 0.60);
    d.steer_linearity = clamp_nan(d.steer_linearity, 0.3, 3.0);
    if (!PAD_PROFILES.contains(d.profile)) d.profile.clear();
    LabelMap fixed_axes;
    for (const auto& name : PAD_DIRECTIONS) {
        auto it = d.axes.find(name);
        fixed_axes[name] = (it != d.axes.end() && contains(PAD_SOURCE_KEYS, it->second)) ? it->second : "";
    }
    d.axes = std::move(fixed_axes);
    ButtonMap fixed;
    for (const auto& key : PAD_BUTTON_NAMES) {
        auto it = d.buttons.find(key);
        int v = it == d.buttons.end() ? -1 : it->second;
        fixed[key] = (v >= 0 && v < protocol::NUM_BUTTONS) ? v : -1;
    }
    d.buttons = std::move(fixed);

    link.baudrate = clamp_int(link.baudrate, 9600, 2'000'000);
    link.report_hz = clamp_int(link.report_hz, 50, 500);
    output.vjoy_device = clamp_int(output.vjoy_device, 1, 16);
    if (!contains(OUTPUT_MODES, output.mode)) output.mode = "g29";

    ButtonMap fixed_g29;
    for (const auto& key : G29_CONTROLS) {
        auto it = g29.buttons.find(key);
        int v = it == g29.buttons.end() ? -1 : it->second;
        fixed_g29[key] = (v >= 0 && v < protocol::NUM_BUTTONS) ? v : -1;
    }
    g29.buttons = std::move(fixed_g29);

    ui.tab = clamp_int(ui.tab, 0, 31);
}

// ------------------------------------------------------------ serializacja

namespace {

Value pedal_to_json(const PedalConfig& p) {
    Value v = Value::object();
    v.set("source", p.source);
    v.set("raw_min", p.raw_min);
    v.set("raw_max", p.raw_max);
    v.set("invert", p.invert);
    v.set("deadzone_low", p.deadzone_low);
    v.set("deadzone_high", p.deadzone_high);
    v.set("gamma", p.gamma);
    v.set("smoothing", p.smoothing);
    v.set("vjoy_axis", p.vjoy_axis);
    return v;
}

Value button_map_to_json(const ButtonMap& m, const Names& order) {
    Value v = Value::object();
    for (const auto& key : order) v.set(key, m.at(key));
    return v;
}

} // namespace

std::string AppConfig::to_json() const {
    Value root = Value::object();
    root.set("version", 1);

    Value l = Value::object();
    l.set("port", link.port);
    l.set("baudrate", link.baudrate);
    l.set("auto_connect", link.auto_connect);
    l.set("report_hz", link.report_hz);
    root.set("link", std::move(l));

    Value o = Value::object();
    o.set("mode", output.mode);
    o.set("vjoy_device", output.vjoy_device);
    o.set("auto_acquire", output.auto_acquire);
    o.set("hide_unused", output.hide_unused);
    root.set("output", std::move(o));

    Value s = Value::object();
    s.set("counts_per_rev", steer.counts_per_rev);
    s.set("range_deg", steer.range_deg);
    s.set("invert", steer.invert);
    s.set("smoothing", steer.smoothing);
    s.set("linearity", steer.linearity);
    s.set("vjoy_axis", steer.vjoy_axis);
    s.set("center_counts", steer.center_counts);
    s.set("soft_endstop", steer.soft_endstop);
    s.set("endstop_gain", steer.endstop_gain);
    root.set("steer", std::move(s));

    Value peds = Value::object();
    for (int i = 0; i < NUM_PEDALS; ++i)
        peds.set(PEDAL_KEYS[static_cast<size_t>(i)], pedal_to_json(pedals[static_cast<size_t>(i)]));
    root.set("pedals", std::move(peds));

    Value f = Value::object();
    f.set("enabled", ffb.enabled);
    f.set("overall_gain", ffb.overall_gain);
    f.set("min_force", ffb.min_force);
    f.set("max_force", ffb.max_force);
    f.set("invert", ffb.invert);
    f.set("deadband", ffb.deadband);
    f.set("slew_per_ms", ffb.slew_per_ms);
    f.set("timeout_ms", ffb.timeout_ms);
    f.set("min_duty", ffb.min_duty);
    f.set("max_duty", ffb.max_duty);
    f.set("update_hz", ffb.update_hz);
    f.set("spring_gain", ffb.spring_gain);
    f.set("damper_gain", ffb.damper_gain);
    f.set("friction_gain", ffb.friction_gain);
    f.set("game_constant_gain", ffb.game_constant_gain);
    f.set("game_spring_gain", ffb.game_spring_gain);
    f.set("game_damper_gain", ffb.game_damper_gain);
    f.set("game_periodic_gain", ffb.game_periodic_gain);
    f.set("direction_mode", ffb.direction_mode);
    root.set("ffb", std::move(f));

    Value b = Value::object();
    Value mapping = Value::array();
    for (int v : buttons.mapping) mapping.push(v);
    b.set("mapping", std::move(mapping));
    Value hat = Value::array();
    for (int v : buttons.hat) hat.push(v);
    b.set("hat", std::move(hat));
    Value inv = Value::array();
    for (bool v : buttons.invert) inv.push(Value(v));
    b.set("invert", std::move(inv));
    root.set("buttons", std::move(b));

    Value d = Value::object();
    d.set("steer_range_deg", pad.steer_range_deg);
    d.set("steer_deadzone", pad.steer_deadzone);
    d.set("steer_antideadzone", pad.steer_antideadzone);
    d.set("trigger_antideadzone", pad.trigger_antideadzone);
    d.set("steer_linearity", pad.steer_linearity);
    d.set("profile", pad.profile);
    d.set("steer_invert", pad.steer_invert);
    Value axes = Value::object();
    for (const auto& name : PAD_DIRECTIONS) axes.set(name, pad.axes.at(name));
    d.set("axes", std::move(axes));
    d.set("buttons", button_map_to_json(pad.buttons, PAD_BUTTON_NAMES));
    root.set("pad", std::move(d));

    Value g = Value::object();
    g.set("pedals_inverted", g29.pedals_inverted);
    g.set("buttons", button_map_to_json(g29.buttons, G29_CONTROLS));
    root.set("g29", std::move(g));

    Value u = Value::object();
    u.set("geometry", ui.geometry);
    u.set("zoomed", ui.zoomed);
    u.set("tab", ui.tab);
    root.set("ui", std::move(u));

    return json::dump(root);
}

namespace {

bool busy_error(DWORD e) { return e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION; }

void write_file(const fs::path& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Nie można zapisać " + narrow(path.wstring()) + ": " +
                                 win_error_text(GetLastError()));
    DWORD written = 0;
    BOOL ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    DWORD err = GetLastError();
    CloseHandle(h);
    if (!ok || written != text.size())
        throw std::runtime_error("Nie można zapisać " + narrow(path.wstring()) + ": " + win_error_text(err));
}

} // namespace

fs::path AppConfig::save(const std::optional<fs::path>& path_opt) const {
    fs::path path = path_opt ? *path_opt : config_path();
    fs::path tmp = path;
    tmp.replace_extension(".json.tmp");
    write_file(tmp, to_json());
    // Atomowy zapis - brak uszkodzonego pliku. Na Windowsie podmiana nie
    // udaje sie, gdy ktos akurat czyta stary plik (klient Nextcloud,
    // antywirus) - chwile czekamy i probujemy znowu.
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return path;
        DWORD err = GetLastError();
        if (!busy_error(err) || attempt == SAVE_ATTEMPTS - 1) {
            DeleteFileW(tmp.c_str());
            throw std::runtime_error("Nie można podmienić " + narrow(path.wstring()) + ": " + win_error_text(err));
        }
        Sleep(100);
    }
}

namespace {

enum class ReadResult { Ok, Missing, Failed };

// Tresc pliku; chwilowo zajety plik (Nextcloud, antywirus) - ponawiamy.
ReadResult read_retry(const fs::path& path, std::string& out, DWORD& error) {
    for (int attempt = 0;; ++attempt) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return ReadResult::Missing;
            if (busy_error(error) && attempt < SAVE_ATTEMPTS - 1) {
                Sleep(100);
                continue;
            }
            return ReadResult::Failed;
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        BOOL ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
        error = GetLastError();
        CloseHandle(h);
        if (!ok) return ReadResult::Failed;
        out.resize(got);
        return ReadResult::Ok;
    }
}

// Odklada nieczytelny plik obok i zwraca opis dla dziennika.
std::string keep_bad_copy(const fs::path& path, const std::string& what) {
    fs::path bad = path;
    bad.replace_extension(".json.bad");
    std::error_code ec;
    fs::copy_file(path, bad, fs::copy_options::overwrite_existing, ec);
    std::string where = ec ? "kopii nie udało się zrobić" : "kopia: " + narrow(bad.filename().wstring());
    return narrow(path.filename().wstring()) + " jest uszkodzony (" + what + ") - " + where +
           ", panel startuje z ustawieniami domyślnymi";
}

void read_int(const Value& o, const char* key, int& field) {
    if (const Value* v = o.find(key))
        field = static_cast<int>(std::max<int64_t>(INT_MIN, std::min<int64_t>(INT_MAX, v->to_int(field))));
}
void read_double(const Value& o, const char* key, double& field) {
    if (const Value* v = o.find(key)) field = v->to_double();  // NaN = smieci, normalize da dolna granice
}
void read_bool(const Value& o, const char* key, bool& field) {
    if (const Value* v = o.find(key)) field = v->truthy();
}
void read_string(const Value& o, const char* key, std::string& field) {
    if (const Value* v = o.find(key)) field = v->to_string_or("");
}

// Zepsuta sekcja (np. "steer": null) dostaje wartosci domyslne.
const Value* section(const Value& root, const char* key) {
    const Value* v = root.find(key);
    return (v && v->is_object()) ? v : nullptr;
}

void read_pedal(const Value& o, PedalConfig& p) {
    if (const Value* v = o.find("source"))
        p.source = static_cast<int>(std::max<int64_t>(-1000, std::min<int64_t>(1000, v->to_int(-1))));
    read_int(o, "raw_min", p.raw_min);
    read_int(o, "raw_max", p.raw_max);
    read_bool(o, "invert", p.invert);
    read_double(o, "deadzone_low", p.deadzone_low);
    read_double(o, "deadzone_high", p.deadzone_high);
    read_double(o, "gamma", p.gamma);
    read_double(o, "smoothing", p.smoothing);
    read_string(o, "vjoy_axis", p.vjoy_axis);
}

ButtonMap read_button_map(const Value& v) {
    ButtonMap out;
    if (v.is_object())
        for (const auto& [key, item] : v.members())
            out[key] = static_cast<int>(std::max<int64_t>(-1, std::min<int64_t>(1000, item.to_int(-1))));
    return out;
}

// Przepisuje stare pola `<zrodlo>_target` na gniazda kierunkow.
LabelMap pad_axes_from_legacy(const Value& raw_pad) {
    LabelMap axes;
    std::vector<std::string> keys = {"steer"};
    keys.insert(keys.end(), PEDAL_KEYS.begin(), PEDAL_KEYS.end());
    for (const auto& key : keys) {
        const Value* t = raw_pad.find(key + "_target");
        if (!t || !t->is_string()) continue;
        const std::string& target = t->as_string();
        if (!contains(LEGACY_PAD_TARGETS, target) || target == "-") continue;
        bool trigger = target == "LT" || target == "RT";
        if (key == "steer") {
            if (trigger) {
                // skret na spuscie byl mapowany symetrycznie na 0..1; nowy
                // model nie ma takiej polowki, wiec zostaje sam prawy skret
                axes[target] = "steer_right";
            } else {
                axes[target + "-"] = "steer_left";
                axes[target + "+"] = "steer_right";
            }
        } else {
            // pedal daje 0..1, czyli dodatnia polowke osi
            axes[trigger ? target : target + "+"] = key;
        }
    }
    return axes;
}

AppConfig from_json(const Value& raw) {
    AppConfig cfg;

    if (const Value* o = section(raw, "link")) {
        LinkConfig d;
        read_string(*o, "port", d.port);
        read_int(*o, "baudrate", d.baudrate);
        read_bool(*o, "auto_connect", d.auto_connect);
        read_int(*o, "report_hz", d.report_hz);
        cfg.link = d;
    }
    if (const Value* o = section(raw, "output")) {
        OutputConfig d;
        read_string(*o, "mode", d.mode);
        read_int(*o, "vjoy_device", d.vjoy_device);
        read_bool(*o, "auto_acquire", d.auto_acquire);
        read_bool(*o, "hide_unused", d.hide_unused);
        cfg.output = d;
    }
    if (const Value* o = section(raw, "steer")) {
        SteerConfig d;
        read_int(*o, "counts_per_rev", d.counts_per_rev);
        read_int(*o, "range_deg", d.range_deg);
        read_bool(*o, "invert", d.invert);
        read_double(*o, "smoothing", d.smoothing);
        read_double(*o, "linearity", d.linearity);
        read_string(*o, "vjoy_axis", d.vjoy_axis);
        if (const Value* v = o->find("center_counts")) d.center_counts = v->to_int(0);
        read_bool(*o, "soft_endstop", d.soft_endstop);
        read_int(*o, "endstop_gain", d.endstop_gain);
        cfg.steer = d;
    }
    if (const Value* o = section(raw, "pedals")) {
        for (int i = 0; i < NUM_PEDALS; ++i) {
            const Value* item = o->find(PEDAL_KEYS[static_cast<size_t>(i)]);
            if (!item) continue;  // brakujacy kanal zostaje z domyslnymi AppConfig()
            PedalConfig p;
            if (item->is_object()) read_pedal(*item, p);
            cfg.pedals[static_cast<size_t>(i)] = p;
        }
    }
    if (const Value* o = section(raw, "ffb")) {
        FfbConfig d;
        read_bool(*o, "enabled", d.enabled);
        read_double(*o, "overall_gain", d.overall_gain);
        read_double(*o, "min_force", d.min_force);
        read_double(*o, "max_force", d.max_force);
        read_bool(*o, "invert", d.invert);
        read_double(*o, "deadband", d.deadband);
        read_int(*o, "slew_per_ms", d.slew_per_ms);
        read_int(*o, "timeout_ms", d.timeout_ms);
        read_int(*o, "min_duty", d.min_duty);
        read_int(*o, "max_duty", d.max_duty);
        read_int(*o, "update_hz", d.update_hz);
        read_double(*o, "spring_gain", d.spring_gain);
        read_double(*o, "damper_gain", d.damper_gain);
        read_double(*o, "friction_gain", d.friction_gain);
        read_double(*o, "game_constant_gain", d.game_constant_gain);
        read_double(*o, "game_spring_gain", d.game_spring_gain);
        read_double(*o, "game_damper_gain", d.game_damper_gain);
        read_double(*o, "game_periodic_gain", d.game_periodic_gain);
        read_string(*o, "direction_mode", d.direction_mode);
        cfg.ffb = d;
    }
    if (const Value* o = section(raw, "buttons")) {
        ButtonsConfig d;
        if (const Value* m = o->find("mapping")) {
            d.mapping.clear();
            if (m->is_array())
                for (size_t i = 0; i < m->items().size() && i < protocol::NUM_BUTTONS; ++i) {
                    int64_t v = m->items()[i].to_int(static_cast<int64_t>(i) + 1);
                    d.mapping.push_back(static_cast<int>(std::max<int64_t>(0, std::min<int64_t>(128, v))));
                }
        }
        if (const Value* h = o->find("hat")) {
            d.hat.clear();
            if (h->is_array())
                for (size_t i = 0; i < h->items().size() && i < 4; ++i) {
                    int64_t v = h->items()[i].to_int(-1);
                    d.hat.push_back(static_cast<int>(std::max<int64_t>(-2, std::min<int64_t>(1000, v))));
                }
        }
        if (const Value* n = o->find("invert")) {
            d.invert.clear();
            if (n->is_array())
                for (size_t i = 0; i < n->items().size() && i < protocol::NUM_BUTTONS; ++i)
                    d.invert.push_back(n->items()[i].truthy());
        }
        cfg.buttons = d;
    }
    if (const Value* o = section(raw, "pad")) {
        PadConfig d;
        read_int(*o, "steer_range_deg", d.steer_range_deg);
        read_double(*o, "steer_deadzone", d.steer_deadzone);
        read_double(*o, "steer_antideadzone", d.steer_antideadzone);
        read_double(*o, "trigger_antideadzone", d.trigger_antideadzone);
        read_double(*o, "steer_linearity", d.steer_linearity);
        read_string(*o, "profile", d.profile);
        read_bool(*o, "steer_invert", d.steer_invert);
        if (const Value* a = o->find("axes")) {
            d.axes.clear();
            if (a->is_object())
                for (const auto& [key, item] : a->members())
                    d.axes[key] = item.to_string_or("");
        }
        if (const Value* b = o->find("buttons")) d.buttons = read_button_map(*b);
        cfg.pad = d;
    }
    if (const Value* o = section(raw, "g29")) {
        G29Config d;
        read_bool(*o, "pedals_inverted", d.pedals_inverted);
        if (const Value* b = o->find("buttons")) d.buttons = read_button_map(*b);
        cfg.g29 = d;
    }
    if (const Value* o = section(raw, "ui")) {
        UiConfig d;
        read_string(*o, "geometry", d.geometry);
        read_bool(*o, "zoomed", d.zoomed);
        read_int(*o, "tab", d.tab);
        cfg.ui = d;
    }
    cfg.normalize();
    return cfg;
}

} // namespace

AppConfig AppConfig::load(const std::optional<fs::path>& path_opt) {
    last_load_problem.clear();
    fs::path path = path_opt ? *path_opt : config_path();

    std::string text;
    DWORD error = 0;
    switch (read_retry(path, text, error)) {
    case ReadResult::Missing:
        return AppConfig();  // pierwsze uruchomienie
    case ReadResult::Failed:
        last_load_problem = "Nie da się odczytać " + narrow(path.filename().wstring()) + " (" +
                            win_error_text(error) + ") - panel startuje z ustawieniami domyślnymi";
        return AppConfig();
    case ReadResult::Ok:
        break;
    }
    if (!json::is_valid_utf8(text)) {
        last_load_problem = keep_bad_copy(path, "UnicodeDecodeError: plik nie jest w UTF-8");
        return AppConfig();
    }

    Value raw;
    AppConfig cfg;
    try {
        raw = json::parse(text);
        if (!raw.is_object()) throw json::ParseError("w pliku nie ma obiektu JSON");
        cfg = from_json(raw);
    } catch (const std::exception& exc) {
        last_load_problem = keep_bad_copy(path, std::string("JSONDecodeError: ") + exc.what());
        return AppConfig();
    }

    // Migracja z wersji sprzed trybu pada: pole output.backend
    if (const Value* out = section(raw, "output")) {
        if (!out->contains("mode") && out->contains("backend")) {
            const Value* b = out->find("backend");
            cfg.output.mode = (b->is_string() && b->as_string() == "none") ? "none" : "wheel";
        }
    }
    // Migracja ze starego modelu osi pada (jedno zrodlo -> cala os).
    if (const Value* pad_raw = section(raw, "pad")) {
        if (!pad_raw->contains("axes")) {
            LabelMap legacy = pad_axes_from_legacy(*pad_raw);
            cfg.pad.axes = legacy.empty() ? DEFAULT_PAD_AXES : legacy;
        }
    }
    // Plik sprzed wlasnych przypisan G29 - przenosimy przypisania pada.
    const Value* g29_raw = section(raw, "g29");
    if (!(g29_raw && g29_raw->contains("buttons"))) cfg.g29.buttons = g29_buttons_from_pad(cfg.pad.buttons);
    cfg.normalize();
    return cfg;
}

} // namespace wb

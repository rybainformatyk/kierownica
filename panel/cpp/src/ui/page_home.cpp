// Pulpit: kokpit na zywo, telemetria, wybor trybu i stan systemu.
#include <algorithm>
#include <cmath>
#include <format>

#include "app.h"
#include "../util.h"

namespace wb::ui {

const std::array<ModeInfo, 6> MODE_INFO = {{
    {"g29", ic::car, "Kierownica G29", "Każda gra z kierownicą"},
    {"g29_vjoy", ic::add, "G29 + vJoy", "Jazda na G29, przyciski z vJoy"},
    {"wheel", ic::bolt, "Kierownica vJoy", "Symulatory, Force Feedback"},
    {"pad", ic::game, "Pad Xbox 360", "Gry XInput, np. NFS"},
    {"both", ic::link, "Oba naraz", "Kierownica i pad"},
    {"none", ic::eye, "Tylko podgląd", "Nic nie wysyłaj"},
}};

const std::array<RGB, NUM_PEDALS> PEDAL_COLORS = {GOOD, BAD, WARN, ACCENT, ACCENT, ACCENT};
const std::array<int, 7> RANGE_PRESETS = {120, 270, 360, 540, 720, 900, 1080};

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

std::string hex_bytes(const uint8_t* data, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += std::format("{}{:02x}", i ? " " : "", data[i]);
    return out;
}

int index_of(const Names& list, const std::string& value, int fallback) {
    auto it = std::find(list.begin(), list.end(), value);
    return it == list.end() ? fallback : static_cast<int>(it - list.begin());
}

std::unique_ptr<Label> note(std::string text, float cap) { return mk<Label>(std::move(text), Font::Small, MUTED, cap); }

// --------------------------------------------------------------------- Page

Slider* Page::bind_slider(Box* body, std::string title, std::string desc, double from, double to, double* field, Unit unit,
                          double resolution, std::optional<double> def, std::function<void()> after) {
    auto slider = mk<Slider>(from, to, *field, unit, resolution,
                             [this, field, after](double v) {
                                 *field = v;
                                 app_.cfg().normalize();
                                 if (after) after();
                             },
                             def);
    Slider* raw = slider.get();
    bool divider = !body->empty();
    body->add(mk<SettingRow>(std::move(title), std::move(desc), std::move(slider), 320.0f, divider));
    syncs_.push_back([raw, field] { raw->set(*field); });
    return raw;
}

Slider* Page::bind_slider_int(Box* body, std::string title, std::string desc, double from, double to, int* field, Unit unit,
                              double resolution, std::optional<double> def, std::function<void()> after) {
    auto slider = mk<Slider>(from, to, *field, unit, resolution,
                             [this, field, after](double v) {
                                 *field = static_cast<int>(v);
                                 app_.cfg().normalize();
                                 if (after) after();
                             },
                             def);
    Slider* raw = slider.get();
    bool divider = !body->empty();
    body->add(mk<SettingRow>(std::move(title), std::move(desc), std::move(slider), 320.0f, divider));
    syncs_.push_back([raw, field] { raw->set(*field); });
    return raw;
}

Toggle* Page::bind_toggle(Box* body, std::string title, std::string desc, bool* field, std::function<void()> after) {
    auto toggle = mk<Toggle>(*field, [field, after](bool v) {
        *field = v;
        if (after) after();
    });
    Toggle* raw = toggle.get();
    bool divider = !body->empty();
    body->add(mk<SettingRow>(std::move(title), std::move(desc), std::move(toggle), 0.0f, divider));
    syncs_.push_back([raw, field] { raw->set(*field); });
    return raw;
}

void Page::add_row(Box* body, std::string title, std::string desc, WidgetPtr control, float control_w) {
    bool divider = !body->empty();
    body->add(mk<SettingRow>(std::move(title), std::move(desc), std::move(control), control_w, divider));
}

void Page::sync() {
    for (auto& s : syncs_) s();
}

// ----------------------------------------------------------------- HomePage

namespace {

// Dziecko nie szersze niz podana szerokosc.
class Capped final : public Widget {
public:
    Capped(WidgetPtr child, float max_w) : child_(std::move(child)), max_w_(max_w) {}
    Size measure(float w) override { return child_->measure(std::min(w, max_w_)); }
    void arrange(const Rect& rc) override {
        r = rc;
        child_->arrange({rc.x, rc.y, std::min(rc.w, max_w_), rc.h});
    }
    void paint(Gfx& g) override { child_->paint(g); }
    Widget* hit_test(float x, float y) override { return child_->hit_test(x, y); }

private:
    WidgetPtr child_;
    float max_w_;
};

} // namespace

std::unique_ptr<Widget> HomePage::build() {
    auto page = mk<Box>(false);
    AppConfig& cfg = app_.cfg();

    // --- kokpit: kierownica, odczyty i pedaly ---
    auto hero = mk<Card>("");
    constexpr float PEDALS_W = 368.0f;
    // w waskim oknie pedaly schodza pod kierownice
    auto row = mk<Columns>(std::vector<float>{1.0f, -PEDALS_W}, 520.0f, 32.0f);
    auto wheel = mk<Box>(true);
    dial_ = wheel->add(mk<WheelDial>(264.0f), Pack{.right = 28, .cross = Pack::Center});
    dial_->set_range(cfg.steer.range_deg);

    auto info = mk<Box>(false);
    info->add(mk<Label>("KĄT KIEROWNICY", Font::Caps, MUTED));
    auto angle = mk<Label>("0°", Font::NumGiant, TEXT);
    angle->set_fixed_width(230);
    angle_ = info->add(std::move(angle), Pack{.top = 2});
    auto range = mk<Label>("zakres 900°", Font::Small, MUTED);
    range->set_fixed_width(230);
    range_ = info->add(std::move(range), Pack{.bottom = 14});
    counts_ = info->add(mk<StatRow>("Impulsy enkodera"));
    axis_ = info->add(mk<StatRow>("Oś wyjściowa"));
    velocity_ = info->add(mk<StatRow>("Prędkość obrotu"));
    wheel->add(mk<Capped>(std::move(info), 400.0f), Pack{.flex = 1, .cross = Pack::Center});
    row->add(std::move(wheel));

    auto pedals = mk<Box>(true);
    pedals->add_spacer();
    for (size_t i = 0; i < NUM_PEDALS; ++i) {
        bool main_pedal = i < 3;
        bars_[i] = pedals->add(mk<PedalBar>(PEDAL_LABELS.at(PEDAL_KEYS[i]), PEDAL_COLORS[i], main_pedal ? 60.0f : 38.0f, 176.0f),
                               Pack{.left = i == 0 ? 0.0f : (i == 3 ? 26.0f : 12.0f), .cross = Pack::Center});
    }
    pedals->add_spacer();
    row->add(std::move(pedals));
    hero->body->add(std::move(row));

    // zakres obrotu i srodek: jednym kliknieciem, bez wchodzenia w ustawienia
    auto range_row = mk<Box>(true);
    range_row->add(mk<Label>("Zakres", Font::SmallStrong, TEXT_DIM), Pack{.right = 14, .cross = Pack::Center});
    std::vector<std::string> labels;
    int selected = -1;
    for (size_t i = 0; i < RANGE_PRESETS.size(); ++i) {
        labels.push_back(std::format("{}°", RANGE_PRESETS[i]));
        if (RANGE_PRESETS[i] == cfg.steer.range_deg) selected = static_cast<int>(i);
    }
    presets_ = range_row->add(mk<Segmented>(labels, selected, [this](int i) { app_.set_range(RANGE_PRESETS[static_cast<size_t>(i)]); }), Pack{.flex = 1, .cross = Pack::Center});
    auto center = mk<Button>("Ustaw środek", Button::Accent, [this] { app_.engine().center_wheel(); });
    center->set_icon(ic::target);
    range_row->add(std::move(center), Pack{.left = 16, .cross = Pack::Center});
    range_row->add(mk<Button>("Reset płytki", Button::Normal, [this] { app_.reset_board(); }), Pack{.left = 8, .cross = Pack::Center});
    hero->body->add(std::move(range_row), Pack{.top = 14});
    page->add(std::move(hero), Pack{.bottom = 16});

    // --- telemetria ---
    auto chart = mk<Card>("");
    std::vector<TraceChart::Series> series = {{"Skręt", ACCENT, 0, false},  {"Moment FFB", VIOLET, 0, false}, {"Gaz", GOOD, 1, true},
                                              {"Hamulec", BAD, 1, true},    {"Sprzęgło", WARN, 1, false}};
    std::vector<TraceChart::Lane> lanes = {{"SKRĘT I MOMENT", true, 1.0f}, {"PEDAŁY", false, 1.0f}};
    trace_ = chart->body->add(mk<TraceChart>(std::move(series), std::move(lanes), 214.0f));
    page->add(std::move(chart), Pack{.bottom = 16});

    // --- wyjscie, stan i przyciski ---
    auto cols = mk<Columns>(std::vector<float>{1.25f, 1.05f, 1.15f}, 330.0f, 16.0f);

    auto out_card = mk<Card>("Wyjście do gier");
    auto tiles = mk<Grid>(std::vector<float>{1, 1});
    for (size_t i = 0; i < MODE_INFO.size(); ++i) {
        const ModeInfo& m = MODE_INFO[i];
        modes_[i] = tiles->place(mk<ModeTile>(m.icon, m.title, m.desc, [this, key = std::string(m.key)] { app_.set_mode(key); }),
                                 static_cast<int>(i) / 2, static_cast<int>(i) % 2, 1, Pack{.left = 3, .top = 3, .right = 3, .bottom = 3});
    }
    out_card->body->add(std::move(tiles));
    output_text_ = out_card->body->add(mk<Label>("-", Font::Small, TEXT_DIM, 2000.0f), Pack{.top = 10});
    cols->add(std::move(out_card));

    auto status = mk<Card>("Stan systemu");
    s_board_ = status->body->add(mk<StatusLine>());
    s_fw_ = status->body->add(mk<StatusLine>());
    s_output_ = status->body->add(mk<StatusLine>());
    s_ffb_ = status->body->add(mk<StatusLine>());
    s_flags_ = status->body->add(mk<StatusLine>());
    s_flags_->visible = false;
    hint_ = status->body->add(mk<Label>("", Font::Small, WARN, 2000.0f), Pack{.top = 6});
    hint_->visible = false;
    status->body->add(mk<Label>("MOMENT FORCE FEEDBACK", Font::Caps, MUTED), Pack{.top = 10, .bottom = 6});
    torque_ = status->body->add(mk<TorqueMeter>(24.0f));
    cols->add(std::move(status));

    auto btn_card = mk<Card>("Przyciski");
    auto btn_row = mk<Box>(true);
    grid_ = btn_row->add(mk<ButtonGrid>(), Pack{.flex = 1});
    hat_ = btn_row->add(mk<HatIndicator>(76.0f), Pack{.left = 14, .cross = Pack::Center});
    btn_card->body->add(std::move(btn_row));
    out_ = btn_card->body->add(mk<Label>("", Font::Small, TEXT_DIM, 2000.0f), Pack{.top = 10});
    cols->add(std::move(btn_card));

    page->add(std::move(cols), Pack{.bottom = 8});
    return page;
}

void HomePage::refresh_status(const Frame& f) {
    const EngineStatus& st = f.st;
    AppConfig& cfg = app_.cfg();
    auto hello = app_.engine().hello();

    if (app_.flashing()) {
        s_board_->set(Level::Busy, "Wgrywanie firmware", "Panel połączy się po zamknięciu okna wgrywania");
        s_board_->set_action("", nullptr);
    } else if (f.connected) {
        s_board_->set(Level::Ok, "Płytka połączona", std::format("{} · {} · {:.0f} Hz", hello ? hello->board() : "?", f.stats.port, f.stats.frames_per_sec));
        s_board_->set_action("", nullptr);
    } else if (f.link == LinkState::Scanning) {
        s_board_->set(Level::Busy, "Szukanie płytki", st.link_detail);
        s_board_->set_action("Przerwij", [this] { app_.toggle_connect(); });
    } else {
        s_board_->set(Level::Off, "Płytka rozłączona", "");
        s_board_->set_action("Połącz", [this] { app_.toggle_connect(); });
    }

    if (!hello) {
        s_fw_->set(Level::Off, "Firmware", "Widać po połączeniu z płytką");
        s_fw_->set_action("", nullptr);
    } else if (hello->outdated()) {
        s_fw_->set(Level::Warn, std::format("Firmware {}", hello->firmware()), std::format("Dostępna wersja {}", protocol::FW_LATEST_TEXT));
        s_fw_->set_action("Wgraj", [this] { app_.flash_firmware(); });
    } else {
        s_fw_->set(Level::Ok, std::format("Firmware {}", hello->firmware()), "Aktualny");
        s_fw_->set_action("", nullptr);
    }

    auto [summary, level] = app_.output_summary(f.connected, st);
    if (cfg.output.mode == "none") {
        s_output_->set(Level::Off, "Tryb podglądu", "Gry nie widzą kierownicy");
        s_output_->set_action("", nullptr);
    } else {
        if (level == 0) s_output_->set(Level::Ok, "Gry widzą: " + summary, "Wyjście działa");
        else if (level == 1) s_output_->set(Level::Busy, "Wyjście uruchamia się", summary);
        else s_output_->set(Level::Bad, "Gry nie widzą wyjścia", summary);
        if (level == 2 && (cfg.output.wheel_on() || cfg.output.pad_on())) s_output_->set_action("Podłącz ponownie", [this] { app_.reopen_output(true); });
        else s_output_->set_action("", nullptr);
    }

    if (cfg.ffb.enabled) {
        s_ffb_->set(Level::Ok, "Force Feedback włączony", app_.engine().ffb().registered() ? "Odbiór efektów z gry aktywny" : "Tylko efekty lokalne");
        s_ffb_->set_action("", nullptr);
    } else {
        s_ffb_->set(Level::Off, "Force Feedback wyłączony", "Silnik zwolniony");
        s_ffb_->set_action("Ustawienia", [this] { app_.go_tab(TAB_FFB); });
    }

    std::vector<std::string> flags;
    if (f.live) {
        if (f.state->flags & protocol::FLAG_ENCODER_ERROR) flags.push_back("błędy kwadratury enkodera");
        if (f.state->flags & protocol::FLAG_ENDSTOP_HIT) flags.push_back("na ograniczniku");
        if (f.state->flags & protocol::FLAG_FFB_WATCHDOG) flags.push_back("watchdog FFB zadziałał");
    }
    if (!flags.empty()) s_flags_->set(Level::Warn, "Uwaga", join(flags, ", "));
    s_flags_->set_visible(!flags.empty());

    std::string hint = app_.output_hint(f.connected, st);
    hint_->set(hint);
    hint_->set_visible(!hint.empty());
}

void HomePage::refresh(const Frame& f, bool visible) {
    AppConfig& cfg = app_.cfg();

    // historia telemetrii biegnie takze na innych zakladkach
    if (f.live) {
        std::array<float, TraceChart::MAX_SERIES> values{};
        values[0] = static_cast<float>(std::max(-1.0, std::min(1.0, f.state->steer_axis)));
        values[1] = static_cast<float>(f.st.ffb_torque);
        values[2] = static_cast<float>(f.state->pedals[0]);
        values[3] = static_cast<float>(f.state->pedals[1]);
        values[4] = static_cast<float>(f.state->pedals[2]);
        trace_->push(f.now, values, visible);
    }
    if (!visible) return;
    trace_->set_live(f.live);

    dial_->set_range(cfg.steer.range_deg);
    // liczba dogania nowy zakres razem ze skala tarczy
    bool settling = std::abs(dial_->shown_range() - cfg.steer.range_deg) >= 0.5;
    std::string turns = std::format("{:.2f}", dial_->shown_range() / 360.0);
    std::replace(turns.begin(), turns.end(), '.', ',');
    range_->set(std::format("zakres {:.0f}°  ·  {} obrotu", dial_->shown_range(), turns), settling ? ACCENT_HI : MUTED);
    int selected = -1;
    for (size_t i = 0; i < RANGE_PRESETS.size(); ++i)
        if (RANGE_PRESETS[i] == cfg.steer.range_deg) selected = static_cast<int>(i);
    presets_->set_index(selected);

    if (f.live) {
        const ProcessedState& s = *f.state;
        dial_->set_angle(s.steer_deg);
        double half = std::max(1.0, cfg.steer.range_deg / 2.0);
        angle_->set(std::abs(s.steer_deg) < 0.5 ? "0°" : std::format("{:+.0f}°", s.steer_deg), std::abs(s.steer_deg) > half + 0.5 ? BAD : TEXT);
        counts_->set(std::format("{:+d}", s.raw_counts));
        axis_->set(std::format("{:+.3f}", s.steer_axis));
        velocity_->set(std::format("{:+.0f} st/s", s.steer_velocity));
        for (size_t i = 0; i < NUM_PEDALS; ++i) {
            int src = cfg.pedals[i].source;
            std::optional<int> raw = (src >= 0 && src < protocol::NUM_AXES) ? std::optional<int>(s.raw_axes[static_cast<size_t>(src)]) : std::nullopt;
            bars_[i]->set(s.pedals[i], raw, src >= 0);
        }
        // Pokazujemy stan PO odwroceniu (NC).
        grid_->set_mask(s.buttons_effective);
        hat_->set(s.hat);
        out_->set(app_.output_buttons_text(s).second);
    } else if (!f.connected) {
        dial_->set_angle(0.0);
        angle_->set("—", MUTED);
        counts_->set("-");
        axis_->set("-");
        velocity_->set("-");
        grid_->set_mask(0);
        hat_->set(-1);
        out_->set("");
        for (size_t i = 0; i < NUM_PEDALS; ++i) bars_[i]->set(0.0, std::nullopt, cfg.pedals[i].source >= 0);
    }

    for (size_t i = 0; i < MODE_INFO.size(); ++i) modes_[i]->set_selected(cfg.output.mode == MODE_INFO[i].key);

    std::vector<std::string> out_lines;
    if (cfg.output.g29_on()) out_lines.push_back(f.st.output_text);
    if (cfg.output.wheel_on()) {
        // bez dzialajacego vJoy silnik ma podglad - jego opis "gry nie widza
        // kierownicy" klamal w trybie G29 + vJoy
        if (f.st.output_ok) out_lines.push_back(app_.engine().output_status_text());
        else if (f.st.vjoy_switching) out_lines.push_back("vJoy - czekam na włączenie sterownika");
        else out_lines.push_back("vJoy nieaktywne");
    }
    if (cfg.output.pad_on()) out_lines.push_back(f.st.pad_text);
    if (out_lines.empty()) out_lines.push_back("Tryb podglądu - gry nie widzą kierownicy");
    output_text_->set(join(out_lines, "\n"));

    refresh_status(f);
    torque_->set(f.st.ffb_torque);
}

} // namespace wb::ui

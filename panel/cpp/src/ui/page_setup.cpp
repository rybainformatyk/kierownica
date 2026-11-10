// Zakladki ustawien: kierownica, pedaly, przyciski i Force Feedback.
#include <algorithm>
#include <cmath>
#include <format>

#include "app.h"
#include "../json.h"
#include "../util.h"

namespace wb::ui {

namespace {

constexpr Unit DEG{0, "°", 1.0};
constexpr Unit PLAIN{0, "", 1.0};
constexpr Unit TWO{2, "", 1.0};
constexpr Unit PCT{0, "%", 100.0};
constexpr Unit PCT1{1, "%", 100.0};

std::vector<std::string> axis_labels() { return std::vector<std::string>(VJOY_AXIS_CHOICES.begin(), VJOY_AXIS_CHOICES.end()); }

std::vector<std::string> preset_labels() {
    std::vector<std::string> out;
    for (int deg : RANGE_PRESETS) out.push_back(std::format("{}°", deg));
    return out;
}

int preset_index(int degrees) {
    for (size_t i = 0; i < RANGE_PRESETS.size(); ++i)
        if (RANGE_PRESETS[i] == degrees) return static_cast<int>(i);
    return -1;
}

double snap_to(double v, double step, double lo, double hi) {
    // przez 1e6, zeby do config.json nie trafialo 0.6000000000000001
    return std::max(lo, std::min(hi, std::rint(std::rint(v / step) * step * 1e6) / 1e6));
}

} // namespace

// ---------------------------------------------------------------- WheelPage

std::unique_ptr<Widget> WheelPage::build() {
    AppConfig& cfg = app_.cfg();
    const SteerConfig d;
    auto cols = mk<Columns>(std::vector<float>{1, 1}, 420.0f);

    // --- lewa kolumna ---
    auto left = mk<Box>(false);
    auto c1 = mk<Card>("Zakres obrotu");
    presets_ = c1->body->add(mk<Segmented>(preset_labels(), preset_index(cfg.steer.range_deg), [this](int i) { app_.set_range(RANGE_PRESETS[static_cast<size_t>(i)]); }), Pack{.bottom = 12});
    ruler_ = c1->body->add(mk<RangeRuler>(), Pack{.bottom = 6});
    ruler_->set_range(cfg.steer.range_deg);
    range_ = bind_slider_int(c1->body, "Zakres lock-to-lock", "Tyle stopni ma pełny obrót od oporu do oporu. G29 ma fabrycznie 900°.", 90, 1800, &cfg.steer.range_deg, DEG, 10, d.range_deg,
                             [this] { app_.engine().apply_config(); });
    left->add(std::move(c1), Pack{.bottom = 16});

    auto c2 = mk<Card>("Enkoder");
    auto row = mk<Box>(true);
    cpr_ = row->add(mk<Entry>(std::to_string(cfg.steer.counts_per_rev), 10), Pack{.cross = Pack::Center});
    row->add(mk<Button>("Zapisz", Button::Small, [this] { apply_cpr(); }), Pack{.left = 8, .cross = Pack::Center});
    add_row(c2->body, "Impulsy na obrót kierownicy", "Liczone x4, jak podaje enkoder.", std::move(row));
    add_row(c2->body, "Wykrywanie automatyczne", "Ustaw środek, przekręć kierownicę do jednego oporu, potem do drugiego i kliknij przycisk. Aplikacja policzy impulsy z przejechanego zakresu.",
            mk<Button>("Wykryj", Button::Normal, [this] { detect_cpr(); }));
    captured_ = c2->body->add(mk<Label>("", Font::Small, TEXT_DIM), Pack{.top = 2, .cross = Pack::Start});
    left->add(std::move(c2), Pack{.bottom = 16});

    auto c3 = mk<Card>("Miękki ogranicznik końca zakresu", "Działa w firmware, niezależnie od gry - przy przekroczeniu zakresu silnik odpycha kierownicę. Wymaga podłączonego mostka H.", 2000.0f);
    bind_toggle(c3->body, "Włącz miękki ogranicznik", "", &cfg.steer.soft_endstop, [this] { app_.engine().apply_config(); });
    bind_slider_int(c3->body, "Siła ogranicznika", "Moment na każdy impuls przekroczenia zakresu.", 1, 200, &cfg.steer.endstop_gain, PLAIN, 1, d.endstop_gain, [this] { app_.engine().apply_config(); });
    left->add(std::move(c3));
    cols->add(std::move(left));

    // --- prawa kolumna ---
    auto c4 = mk<Card>("Charakterystyka");
    CurveEditor::Handlers h;
    h.gamma = [this](double v) {
        app_.cfg().steer.linearity = snap_to(v, 0.05, 0.4, 2.5);
        app_.cfg().normalize();
    };
    curve_ = c4->body->add(mk<CurveEditor>(232.0f, ACCENT, true, std::array<std::string, 3>{"", "", ""}, h), Pack{.bottom = 6});
    auto live = mk<Grid>(std::vector<float>{1, 1});
    angle_ = live->place(mk<StatRow>("Kąt"), 0, 0, 1, Pack{.right = 12});
    axis_ = live->place(mk<StatRow>("Oś w grze"), 0, 1, 1, Pack{.left = 12});
    c4->body->add(std::move(live), Pack{.bottom = 6});
    bind_slider(c4->body, "Liniowość", "1.00 = liniowo. Poniżej 1 kierownica reaguje ostrzej przy środku, powyżej 1 łagodniej. Możesz też przeciągnąć krzywą.", 0.4, 2.5, &cfg.steer.linearity, TWO, 0.05, d.linearity);
    bind_slider(c4->body, "Wygładzanie", "Filtr drgań enkodera. Większa wartość = gładziej, ale z większym opóźnieniem. Zwykle 0.", 0.0, 0.9, &cfg.steer.smoothing, TWO, 0.05, d.smoothing);
    bind_toggle(c4->body, "Odwróć kierunek obrotu", "", &cfg.steer.invert, [this] {
        app_.engine().publish_config();
        app_.engine().reset_steer_processor();
    });
    auto combo = mk<Combo>(axis_labels(), index_of(VJOY_AXIS_CHOICES, cfg.steer.vjoy_axis),
                           [this](int i) {
                               app_.cfg().steer.vjoy_axis = VJOY_AXIS_CHOICES[static_cast<size_t>(i)];
                               app_.engine().apply_config();
                           },
                           8);
    axis_combo_ = combo.get();
    add_row(c4->body, "Oś vJoy", "G29 używa osi X.", std::move(combo));
    cols->add(std::move(c4));

    auto page = mk<Box>(false);
    page->add(std::move(cols), Pack{.bottom = 8});
    return page;
}

void WheelPage::apply_cpr() {
    json::Value text(cpr_->text());
    int64_t value = text.to_int(INT64_MIN);
    if (value == INT64_MIN) {
        app_.inform("Impulsy enkodera", "Podaj liczbę całkowitą.", MB_ICONWARNING);
        return;
    }
    if (value < 4) {
        app_.inform("Impulsy enkodera", "Wartość musi być co najmniej 4.", MB_ICONWARNING);
        return;
    }
    app_.cfg().steer.counts_per_rev = static_cast<int>(std::min<int64_t>(value, 2'000'000));
    app_.engine().apply_config();
    cpr_->set(std::to_string(app_.cfg().steer.counts_per_rev));
    app_.log("ok", std::format("Impulsy na obrót: {}", app_.cfg().steer.counts_per_rev));
    Ui::get().toast(std::format("Impulsy na obrót: {}", app_.cfg().steer.counts_per_rev));
}

void WheelPage::detect_cpr() {
    auto cpr = app_.engine().detect_counts_per_rev();
    if (!cpr) {
        app_.inform("Wykrywanie enkodera", "Za mały przejechany zakres.\n\nUstaw środek, przekręć kierownicę do jednego oporu, potem do drugiego i kliknij ponownie.", MB_ICONINFORMATION);
        return;
    }
    app_.cfg().steer.counts_per_rev = *cpr;
    cpr_->set(std::to_string(*cpr));
    app_.engine().apply_config();
    app_.log("ok", std::format("Wykryto {} impulsów na obrót kierownicy", *cpr));
    Ui::get().toast(std::format("Wykryto {} impulsów na obrót", *cpr));
}

void WheelPage::refresh(const Frame& f, bool visible) {
    if (!visible) return;
    AppConfig& cfg = app_.cfg();
    sync();
    presets_->set_index(preset_index(cfg.steer.range_deg));
    ruler_->set_range(cfg.steer.range_deg);
    ruler_->set_angle(f.live ? std::optional<double>(f.state->steer_deg) : std::nullopt);
    axis_combo_->set_index(index_of(VJOY_AXIS_CHOICES, cfg.steer.vjoy_axis));
    if (Ui::get().focus() != cpr_) cpr_->set(std::to_string(cfg.steer.counts_per_rev));

    double lin = cfg.steer.linearity;
    double half = std::max(1.0, cfg.steer.range_deg / 2.0);
    curve_->set_curve([lin](double x) { return steer_curve(x, lin); });
    curve_->set_params(0, 0, lin);
    curve_->set_captions(std::format("-{:.0f}°", half), "kąt kierownicy → oś w grze", std::format("+{:.0f}°", half));
    if (f.live) {
        double x = f.state->steer_deg / half;
        curve_->set_marker(std::make_pair(x, steer_curve(x, lin)));
        angle_->set(std::format("{:+.1f}°", f.state->steer_deg));
        axis_->set(std::format("{:+.3f}", f.state->steer_axis));
    } else {
        curve_->set_marker(std::nullopt);
        angle_->set("-");
        axis_->set("-");
    }
    RawSeen seen = app_.engine().raw_seen();
    int64_t span = seen.counts_max - seen.counts_min;
    captured_->set(f.connected ? std::format("Przejechany zakres: {} impulsów", span) : "");
}

// --------------------------------------------------------------- PedalsPage

class PedalsPage::Row final : public Widget {
public:
    Row(std::string name, RGB color, std::function<void()> command) : name_(std::move(name)), color_(color), command_(std::move(command)) {}

    void set(double value, const std::string& channel, bool enabled, bool selected, bool captured) {
        if (std::abs(value - value_) > 0.002 || channel != channel_ || enabled != enabled_ || selected != selected_ || captured != captured_) {
            value_ = value;
            channel_ = channel;
            enabled_ = enabled;
            selected_ = selected;
            captured_ = captured;
            invalidate();
        }
    }

    Size measure(float w) override { return {w, 58}; }
    bool hand_cursor() const override { return true; }
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_up(float x, float y) override {
        if (r.contains(x, y) && command_) command_();
    }

    void paint(Gfx& g) override {
        hover_anim_.set(hover_ ? 1.0f : 0.0f);
        animate(hover_anim_, 22);
        float h = hover_anim_.value;
        if (selected_) {
            g.fill_rrect(r, 10, PANEL_HI);
            g.stroke_rrect(r, 10, mix(BORDER, color_, 0.55));
        } else if (h > 0.01f) {
            g.fill_rrect(r, 10, PANEL_HI, h * 0.7f);
        }
        g.fill_circle(r.x + 16, r.y + 20, 4, enabled_ ? color_ : MUTED);
        g.text({r.x + 28, r.y + 10, r.w - 90, 20}, name_, Font::BodyStrong, enabled_ ? TEXT : MUTED);
        float name_w = g.measure(name_, Font::BodyStrong).width;
        g.text({r.x + 28 + name_w + 8, r.y + 10, 70, 20}, channel_, Font::Tiny, MUTED);
        std::string pct = enabled_ ? std::format("{:.0f}%", value_ * 100) : "wył.";
        g.text({r.x, r.y + 10, r.w - 14, 20}, pct, Font::Num, enabled_ ? TEXT : MUTED, Align::Right);
        if (captured_) g.text({r.right() - 72, r.y + 10, 18, 20}, ic::check, Font::IconSmall, GOOD, Align::Center);
        Rect bar{r.x + 14, r.bottom() - 16, r.w - 28, 5};
        g.fill_rrect(bar, 2.5f, TRACK);
        float w = std::round(bar.w * static_cast<float>(value_));
        if (enabled_ && w >= 2) g.fill_rrect({bar.x, bar.y, w, bar.h}, 2.5f, color_);
    }

private:
    std::string name_;
    RGB color_;
    std::function<void()> command_;
    double value_ = 0;
    std::string channel_;
    bool enabled_ = true, selected_ = false, captured_ = false, hover_ = false;
    Anim hover_anim_;
};

class PedalsPage::Steps final : public Widget {
public:
    explicit Steps(std::vector<std::string> titles) : titles_(std::move(titles)) {}

    void set(int current) {
        if (current_ != current) {
            current_ = current;
            invalidate();
        }
    }

    Size measure(float w) override { return {w, 34}; }
    Widget* hit_test(float, float) override { return nullptr; }

    void paint(Gfx& g) override {
        float w = r.w / static_cast<float>(titles_.size());
        for (size_t i = 0; i < titles_.size(); ++i) {
            float x = r.x + static_cast<float>(i) * w;
            float cy = r.y + r.h / 2;
            int idx = static_cast<int>(i);
            bool done = idx < current_, now = idx == current_;
            RGB color = done ? GOOD : (now ? ACCENT : MUTED);
            g.fill_circle(x + 13, cy, 13, color, now ? 0.22f : 0.12f);
            if (done) g.text({x, cy - 13, 26, 26}, ic::check, Font::IconSmall, color, Align::Center);
            else g.text({x, cy - 13, 26, 26}, std::to_string(i + 1), Font::Num, color, Align::Center);
            g.text({x + 34, r.y, w - 44, r.h}, titles_[i], now ? Font::BodyStrong : Font::Body, now || done ? TEXT : TEXT_DIM);
        }
    }

private:
    std::vector<std::string> titles_;
    int current_ = 0;
};

PedalConfig& PedalsPage::cur() { return app_.cfg().pedals[static_cast<size_t>(selected_)]; }

void PedalsPage::select(int index) {
    selected_ = index;
    invalidate();
}

std::unique_ptr<Widget> PedalsPage::build() {
    auto page = mk<Box>(false);

    auto head = mk<Card>("Kreator kalibracji");
    auto start = mk<Button>("Zacznij nasłuchiwanie", Button::Accent, [this] { calib_start(); });
    btn_listen_ = head->right_slot->add(std::move(start));
    btn_save_ = head->right_slot->add(mk<Button>("Zapisz zakresy", Button::Normal, [this] { calib_apply_all(); }), Pack{.left = 8});
    head->right_slot->add(mk<Button>("Przywróć pełny zakres", Button::Normal, [this] { calib_reset(); }), Pack{.left = 8});
    steps_ = head->body->add(mk<Steps>(std::vector<std::string>{"Zacznij nasłuchiwanie", "Wciśnij każdy pedał do końca i puść", "Zapisz zakresy"}));
    head->body->add(note("Aplikacja zapamiętuje najmniejszą i największą zaobserwowaną wartość każdego kanału."), Pack{.top = 8});
    calib_ = head->body->add(mk<Label>("", Font::Small, TEXT_DIM, 2000.0f), Pack{.top = 8});
    calib_->visible = false;
    page->add(std::move(head), Pack{.bottom = 16});

    auto cols = mk<Columns>(std::vector<float>{-290.0f, 1.0f}, 460.0f);

    auto list = mk<Card>("Kanały");
    for (int i = 0; i < NUM_PEDALS; ++i) {
        size_t s = static_cast<size_t>(i);
        rows_[s] = list->body->add(mk<Row>(PEDAL_LABELS.at(PEDAL_KEYS[s]), PEDAL_COLORS[s], [this, i] { select(i); }), Pack{.bottom = 4});
    }
    cols->add(std::move(list));

    auto detail = mk<Card>("");
    auto header = mk<Box>(true);
    title_ = header->add(mk<Label>("", Font::Title, TEXT), Pack{.cross = Pack::Center});
    header->add_spacer();
    auto pct = mk<Label>("", Font::NumHuge, TEXT);
    pct->set_fixed_width(130);
    pct->set_align(Align::Right);
    pct_ = header->add(std::move(pct), Pack{.cross = Pack::Center});
    detail->body->add(std::move(header), Pack{.bottom = 10});

    CurveEditor::Handlers h;
    h.low = [this](double v) { cur().deadzone_low = v; app_.cfg().normalize(); };
    h.high = [this](double v) { cur().deadzone_high = v; app_.cfg().normalize(); };
    h.gamma = [this](double v) { cur().gamma = snap_to(v, 0.05, 0.3, 3.0); app_.cfg().normalize(); };
    curve_ = detail->body->add(mk<CurveEditor>(226.0f, GOOD, false, std::array<std::string, 3>{"0 %", "wciśnięcie pedału → wyjście do gry", "100 %"}, h), Pack{.bottom = 8});
    range_ = detail->body->add(mk<RangeBar>(GOOD), Pack{.bottom = 4});

    std::vector<std::string> sources = {"wyłączony"};
    for (int a = 0; a < protocol::NUM_AXES; ++a) sources.push_back(std::format("A{}", a));
    auto src = mk<Combo>(sources, 0,
                         [this](int index) {
                             cur().source = index - 1;
                             app_.engine().publish_config();
                             app_.engine().reset_pedal_processor(PEDAL_KEYS[static_cast<size_t>(selected_)]);
                             app_.engine().apply_config();
                         },
                         11);
    src_ = src.get();
    add_row(detail->body, "Kanał", "Wejście analogowe płytki.", std::move(src));
    auto axis = mk<Combo>(axis_labels(), 0,
                          [this](int index) {
                              cur().vjoy_axis = VJOY_AXIS_CHOICES[static_cast<size_t>(index)];
                              app_.engine().apply_config();
                          },
                          8);
    axis_ = axis.get();
    add_row(detail->body, "Oś vJoy", "", std::move(axis));
    auto invert = mk<Toggle>(false, [this](bool v) { cur().invert = v; app_.cfg().normalize(); });
    invert_ = invert.get();
    add_row(detail->body, "Odwróć kierunek", "", std::move(invert));

    // Zakresy suwakow takie same jak w AppConfig::normalize().
    const PedalConfig d;
    auto slider_row = [&](const char* title, const char* desc, double from, double to, Unit unit, double res, double def, double PedalConfig::*member) {
        auto slider = mk<Slider>(from, to, def, unit, res,
                                 [this, member](double v) {
                                     cur().*member = v;
                                     app_.cfg().normalize();
                                 },
                                 def);
        Slider* raw = slider.get();
        add_row(detail->body, title, desc, std::move(slider), 320.0f);
        return raw;
    };
    dz_lo_ = slider_row("Martwa strefa - początek", "Ucina szum na początku skoku.", 0.0, 0.45, PCT, 0.01, d.deadzone_low, &PedalConfig::deadzone_low);
    dz_hi_ = slider_row("Martwa strefa - koniec", "Gwarantuje osiągnięcie 100 %.", 0.0, 0.45, PCT, 0.01, d.deadzone_high, &PedalConfig::deadzone_high);
    gamma_ = slider_row("Krzywa (gamma)", "Poniżej 1 agresywniej na początku, powyżej 1 łagodniej.", 0.3, 3.0, TWO, 0.05, d.gamma, &PedalConfig::gamma);
    smooth_ = slider_row("Wygładzanie", "", 0.0, 0.9, TWO, 0.05, d.smoothing, &PedalConfig::smoothing);
    cols->add(std::move(detail));

    page->add(std::move(cols), Pack{.bottom = 8});
    return page;
}

void PedalsPage::calib_start() {
    app_.engine().reset_raw_tracking();
    listening_ = true;
    calib_step_ = 1;
    calib_->set("Nasłuchiwanie... wciśnij każdy pedał do końca i puść, potem kliknij 'Zapisz zakresy'.");
    calib_->set_visible(true);
    app_.log("info", "Rozpoczęto nasłuchiwanie kalibracji pedałów");
}

void PedalsPage::calib_apply_all() {
    std::vector<std::string> ok, skipped;
    for (const auto& key : PEDAL_KEYS) {
        if (app_.cfg().pedal(key).source < 0) continue;
        (app_.engine().calibrate_pedal_from_seen(key) ? ok : skipped).push_back(PEDAL_LABELS.at(key));
    }
    if (!ok.empty()) app_.log("ok", "Zapisano kalibrację: " + join(ok, ", "));
    if (!skipped.empty()) app_.log("warn", "Za mały zakres, pominięto: " + join(skipped, ", "));
    calib_->set("Zapisano: " + (ok.empty() ? std::string("nic") : join(ok, ", ")) + (skipped.empty() ? "" : " | pominięto: " + join(skipped, ", ")));
    calib_->set_visible(true);
    listening_ = false;
    calib_step_ = ok.empty() ? 0 : 3;
    if (!ok.empty()) Ui::get().toast("Kalibracja zapisana: " + join(ok, ", "));
}

void PedalsPage::calib_reset() {
    for (PedalConfig& p : app_.cfg().pedals) {
        p.raw_min = 0;
        p.raw_max = 4095;
    }
    app_.engine().reset_raw_tracking();
    listening_ = false;
    calib_step_ = 0;
    app_.log("info", "Przywrócono pełny zakres surowy (0-4095)");
}

void PedalsPage::refresh(const Frame& f, bool visible) {
    if (!visible) return;
    AppConfig& cfg = app_.cfg();
    RawSeen seen = app_.engine().raw_seen();

    steps_->set(calib_step_);
    btn_listen_->set_style(listening_ ? Button::Normal : Button::Accent);
    btn_save_->set_style(listening_ ? Button::Accent : Button::Normal);

    for (size_t i = 0; i < NUM_PEDALS; ++i) {
        const PedalConfig& c = cfg.pedals[i];
        bool enabled = c.source >= 0 && c.source < protocol::NUM_AXES;
        bool captured = listening_ && enabled && seen.max[static_cast<size_t>(c.source)] - seen.min[static_cast<size_t>(c.source)] >= 64;
        rows_[i]->set(f.live ? f.state->pedals[i] : 0.0, enabled ? std::format("A{}", c.source) : "", enabled, static_cast<int>(i) == selected_, captured);
    }

    const size_t idx = static_cast<size_t>(selected_);
    const PedalConfig c = cfg.pedals[idx];
    title_->set(PEDAL_LABELS.at(PEDAL_KEYS[idx]));
    bool enabled = c.source >= 0 && c.source < protocol::NUM_AXES;
    pct_->set(!enabled ? "wył." : (f.live ? std::format("{:.0f}%", f.state->pedals[idx] * 100) : "-"), enabled && f.live ? TEXT : MUTED);
    src_->set_index(c.source + 1);
    axis_->set_index(index_of(VJOY_AXIS_CHOICES, c.vjoy_axis));
    invert_->set(c.invert);
    dz_lo_->set(c.deadzone_low);
    dz_hi_->set(c.deadzone_high);
    gamma_->set(c.gamma);
    smooth_->set(c.smoothing);

    curve_->set_color(PEDAL_COLORS[idx]);
    range_->set_color(PEDAL_COLORS[idx]);
    curve_->set_curve([c](double x) { return pedal_curve(x, c); });
    curve_->set_params(c.deadzone_low, c.deadzone_high, c.gamma);

    std::optional<int> raw;
    if (f.live && enabled) raw = f.state->raw_axes[static_cast<size_t>(c.source)];
    std::optional<std::pair<int, int>> seen_range;
    if (enabled) {
        int lo = seen.min[static_cast<size_t>(c.source)], hi = seen.max[static_cast<size_t>(c.source)];
        if (hi >= lo) seen_range = std::make_pair(lo, hi);
    }
    range_->set(c.raw_min, c.raw_max, raw, seen_range);
    if (raw) {
        double pos = pedal_position(*raw, c);
        curve_->set_marker(std::make_pair(pos, pedal_curve(pos, c)));
    } else {
        curve_->set_marker(std::nullopt);
    }
}

// -------------------------------------------------------------- ButtonsPage

class ButtonsPage::Tile final : public Widget {
public:
    Tile(int index, std::function<void(int)> command) : index_(index), command_(std::move(command)) {}

    void set(bool pressed, int target, bool nc, int hat, bool selected) {
        if (pressed != pressed_ || target != target_ || nc != nc_ || hat != hat_ || selected != selected_) {
            pressed_ = pressed;
            target_ = target;
            nc_ = nc;
            hat_ = hat;
            selected_ = selected;
            invalidate();
        }
    }

    Size measure(float w) override { return {w, 66}; }
    bool hand_cursor() const override { return true; }
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_down(float, float) override {
        if (command_) command_(index_);
    }

    void paint(Gfx& g) override {
        lit_.set(pressed_ ? 1.0f : 0.0f);
        animate(lit_, 34);
        float p = lit_.value;
        if (p > 0.01f) g.shadow(r, 11, 10, ACCENT, 0.5f * p);
        RGB fill = mix(hover_ ? ELEV : PANEL_HI, ACCENT, p);
        g.fill_rrect(r, 11, fill);
        g.stroke_rrect(r, 11, selected_ ? ACCENT : (hover_ ? BORDER_HI : BORDER), selected_ ? 2.0f : 1.0f);
        RGB ink = mix(MUTED, ACCENT_INK, p);
        g.text({r.x + 10, r.y + 7, 30, 16}, std::to_string(index_), Font::TinyStrong, ink);
        if (nc_) {
            Rect b{r.right() - 32, r.y + 8, 24, 15};
            g.fill_rrect(b, 7.5f, WARN, p > 0.5f ? 0.9f : 0.2f);
            g.text(b, "NC", Font::Micro, p > 0.5f ? ACCENT_INK : WARN, Align::Center);
        }
        Rect body{r.x, r.y + 22, r.w, r.h - 26};
        RGB main = mix(TEXT, ACCENT_INK, p);
        if (hat_ >= 0) {
            static const char* arrows[4] = {"▲", "▶", "▼", "◀"};
            g.text(body, arrows[hat_], Font::NumLarge, mix(ACCENT, ACCENT_INK, p), Align::Center);
        } else if (target_ > 0) {
            g.text(body, std::to_string(target_), Font::NumLarge, main, Align::Center);
        } else {
            g.text(body, "—", Font::NumLarge, mix(BORDER_HI, ACCENT_INK, p), Align::Center);
        }
    }

private:
    int index_;
    std::function<void(int)> command_;
    bool pressed_ = false, nc_ = false, selected_ = false, hover_ = false;
    int target_ = 0, hat_ = -1;
    Anim lit_;
};

void ButtonsPage::select(int index) {
    selected_ = index;
    invalidate();
}

void ButtonsPage::set_hat(int choice) {
    auto& hat = app_.cfg().buttons.hat;
    for (int& v : hat)
        if (v == selected_) v = -1;
    if (choice > 0) hat[static_cast<size_t>(choice - 1)] = selected_;
    app_.cfg().normalize();
}

std::unique_ptr<Widget> ButtonsPage::build() {
    auto cols = mk<Columns>(std::vector<float>{1.0f, -316.0f}, 480.0f);

    auto left = mk<Card>("Przyciski 0 - 31");
    left->right_slot->add(mk<Button>("Mapowanie 1:1", Button::Small, [this] {
        for (int i = 0; i < protocol::NUM_BUTTONS; ++i) app_.cfg().buttons.mapping[static_cast<size_t>(i)] = i + 1;
        app_.cfg().normalize();
        app_.log("ok", "Przyciski ustawione 1:1");
        Ui::get().toast("Przyciski ustawione 1:1");
    }));
    left->right_slot->add(mk<Button>("Wyczyść wszystko", Button::Small, [this] {
        for (int& v : app_.cfg().buttons.mapping) v = 0;
        app_.cfg().normalize();
        app_.log("info", "Mapowanie przycisków wyczyszczone");
    }), Pack{.left = 6});
    left->right_slot->add(mk<Button>("Zdejmij wszystkie NC", Button::Small, [this] {
        std::fill(app_.cfg().buttons.invert.begin(), app_.cfg().buttons.invert.end(), false);
        app_.cfg().normalize();
        app_.log("info", "Zdjęto odwrócenie ze wszystkich przycisków");
    }), Pack{.left = 6});

    auto grab_row = mk<Box>(true);
    grab_row->add(mk<Label>("Wybieraj przyciskiem na kierownicy", Font::Small, TEXT_DIM), Pack{.right = 10, .cross = Pack::Center});
    grab_ = grab_row->add(mk<Toggle>(true, nullptr), Pack{.cross = Pack::Center});
    grab_row->add_spacer();
    live_ = grab_row->add(mk<Label>("", Font::Small, TEXT_DIM), Pack{.cross = Pack::Center});
    left->body->add(std::move(grab_row), Pack{.bottom = 12});

    auto grid = mk<Grid>(std::vector<float>{1, 1, 1, 1, 1, 1, 1, 1});
    for (int i = 0; i < protocol::NUM_BUTTONS; ++i)
        tiles_[static_cast<size_t>(i)] = grid->place(mk<Tile>(i, [this](int index) { select(index); }), i / 8, i % 8, 1, Pack{.left = 3, .top = 3, .right = 3, .bottom = 3});
    left->body->add(std::move(grid));
    out_ = left->body->add(mk<Label>("", Font::Small, GOOD, 2000.0f), Pack{.top = 12});
    left->body->add(note("Liczba na kafelku to numer przycisku w urządzeniu vJoy; 0 oznacza 'nie wysyłaj'. Kolumna NC odwraca logikę - dla styków normalnie zwartych (zwarty = puszczony). "
                         "Podświetlenie pokazuje stan JUŻ po odwróceniu, więc efekt widać od razu."), Pack{.top = 6});
    cols->add(std::move(left));

    auto right = mk<Box>(false);
    auto inspector = mk<Card>("");
    title_ = inspector->body->add(mk<Label>("", Font::Title, TEXT));
    auto sub = mk<Box>(true);
    pin_ = sub->add(mk<Label>("", Font::Small, MUTED), Pack{.cross = Pack::Center});
    sub->add_spacer();
    state_ = sub->add(mk<Chip>("Puszczony", MUTED), Pack{.cross = Pack::Center});
    inspector->body->add(std::move(sub), Pack{.top = 4, .bottom = 8});

    auto target = mk<Stepper>(0, 128, 0,
                              [this](int v) {
                                  app_.cfg().buttons.mapping[static_cast<size_t>(selected_)] = v;
                                  app_.cfg().normalize();
                              },
                              "nie wysyłaj");
    target_ = target.get();
    inspector->body->add(mk<Label>("Numer w vJoy", Font::BodyStrong, TEXT), Pack{.top = 8, .bottom = 6});
    inspector->body->add(std::move(target), Pack{.cross = Pack::Start});

    auto nc_row = mk<Box>(true);
    auto nc_text = mk<Box>(false);
    nc_text->add(mk<Label>("Styk normalnie zwarty (NC)", Font::BodyStrong, TEXT));
    nc_text->add(note("Zwarty = puszczony."));
    nc_row->add(std::move(nc_text), Pack{.flex = 1, .cross = Pack::Center});
    nc_ = nc_row->add(mk<Toggle>(false, [this](bool v) {
        app_.cfg().buttons.invert[static_cast<size_t>(selected_)] = v;
        app_.cfg().normalize();
    }), Pack{.cross = Pack::Center});
    inspector->body->add(std::move(nc_row), Pack{.top = 16});

    inspector->body->add(mk<Label>("Krzyżak (POV)", Font::BodyStrong, TEXT), Pack{.top = 16});
    inspector->body->add(note("Przycisk przypisany do krzyżaka przestaje być wysyłany jako zwykły przycisk."), Pack{.top = 2, .bottom = 8});
    hat_choice_ = inspector->body->add(mk<Segmented>(std::vector<std::string>{"brak", "▲", "▶", "▼", "◀"}, 0, [this](int i) { set_hat(i); }));
    right->add(std::move(inspector), Pack{.bottom = 16});

    auto hat_card = mk<Card>("Krzyżak");
    auto hat_row = mk<Box>(true);
    hat_ = hat_row->add(mk<HatIndicator>(84.0f), Pack{.right = 16, .cross = Pack::Center});
    auto hat_list = mk<Box>(false);
    const char* names[4] = {"Góra", "Prawo", "Dół", "Lewo"};
    for (size_t i = 0; i < 4; ++i) hat_rows_[i] = hat_list->add(mk<StatRow>(names[i]));
    hat_row->add(std::move(hat_list), Pack{.flex = 1, .cross = Pack::Center});
    hat_card->body->add(std::move(hat_row));
    right->add(std::move(hat_card));
    cols->add(std::move(right));

    auto page = mk<Box>(false);
    page->add(std::move(cols), Pack{.bottom = 8});
    return page;
}

void ButtonsPage::refresh(const Frame& f, bool visible) {
    uint32_t mask = f.live ? f.state->buttons_effective : 0;
    // nowo wcisniety przycisk wybiera swoj kafelek
    if (visible && grab_->value()) {
        uint32_t fresh = mask & ~prev_mask_;
        if (fresh) {
            int index = 0;
            while (!((fresh >> index) & 1)) ++index;
            select(index);
        }
    }
    prev_mask_ = mask;
    if (!visible) return;

    AppConfig& cfg = app_.cfg();
    const auto& hat = cfg.buttons.hat;
    auto hat_of = [&](int button) {
        for (int d = 0; d < 4; ++d)
            if (hat[static_cast<size_t>(d)] == button) return d;
        return -1;
    };
    for (int i = 0; i < protocol::NUM_BUTTONS; ++i) {
        size_t s = static_cast<size_t>(i);
        tiles_[s]->set((mask >> i) & 1, cfg.buttons.mapping[s], cfg.buttons.invert[s], hat_of(i), i == selected_);
    }

    if (f.live) {
        std::vector<std::string> pressed;
        for (int i = 0; i < protocol::NUM_BUTTONS; ++i)
            if ((mask >> i) & 1) pressed.push_back(std::to_string(i));
        live_->set("Wciśnięte teraz: " + (pressed.empty() ? std::string("brak") : join(pressed, ", ")));
        out_->set(app_.output_buttons_text(*f.state).first, GOOD);
    } else {
        live_->set("Wciśnięte teraz: -");
        out_->set(OFFLINE_LIVE, MUTED);
    }

    const size_t s = static_cast<size_t>(selected_);
    title_->set(std::format("Przycisk {}", selected_));
    pin_->set(std::format("pin {}", selected_ == 0 ? 4 : 21 + selected_));
    bool down = (mask >> selected_) & 1;
    state_->set(down ? "Wciśnięty" : "Puszczony", down ? ACCENT : MUTED);
    target_->set(cfg.buttons.mapping[s]);
    nc_->set(cfg.buttons.invert[s]);
    hat_choice_->set_index(hat_of(selected_) + 1);

    hat_->set(f.live ? f.state->hat : -1);
    for (size_t d = 0; d < 4; ++d) hat_rows_[d]->set(hat[d] < 0 ? "—" : std::to_string(hat[d]), hat[d] < 0 ? MUTED : TEXT);
}

// ----------------------------------------------------------------- FfbPage

std::unique_ptr<Widget> FfbPage::build() {
    AppConfig& cfg = app_.cfg();
    FfbConfig& f = cfg.ffb;
    const FfbConfig d;
    auto page = mk<Box>(false);

    auto hero = mk<Card>("");
    auto top = mk<Columns>(std::vector<float>{1.0f, 1.25f}, 360.0f, 28.0f);

    auto left = mk<Box>(false);
    auto switch_row = mk<Box>(true);
    title_ = switch_row->add(mk<Label>("Silnik zwolniony", Font::Title, TEXT), Pack{.flex = 1, .cross = Pack::Center});
    on_ = switch_row->add(mk<Toggle>(f.enabled, [this](bool enabled) {
        app_.engine().set_ffb_enabled(enabled);
        app_.log(enabled ? "ok" : "info", enabled ? "Force Feedback włączony" : "Force Feedback wyłączony");
        Ui::get().toast(enabled ? "Force Feedback włączony" : "Force Feedback wyłączony", enabled ? GOOD : TEXT_DIM, ic::bolt);
    }), Pack{.cross = Pack::Center});
    left->add(std::move(switch_row));
    state_ = left->add(mk<Label>("", Font::Small, TEXT_DIM, 2000.0f), Pack{.top = 6});
    effects_ = left->add(mk<Label>("", Font::Small, VIOLET, 2000.0f), Pack{.top = 4});
    left->add(mk<Label>("Zanim włączysz: Force Feedback steruje silnikiem przez mostek H na pinach 5 (EN), 6 (RPWM) i 7 (LPWM). Jeśli nie masz podłączonego silnika, "
                        "możesz włączyć FFB bezpiecznie - nic się nie stanie. Przy pierwszym uruchomieniu z silnikiem zacznij od małego wzmocnienia i trzymaj ręce na kierownicy.",
                        Font::Small, WARN, 2000.0f), Pack{.top = 12});
    top->add(std::move(left));

    auto right = mk<Box>(false);
    std::vector<TraceChart::Series> series = {{"Moment", VIOLET, 0, true}, {"Skręt", ACCENT, 0, false}};
    std::vector<TraceChart::Lane> lanes = {{"MOMENT WYSYŁANY DO PŁYTKI", true, 1.0f}};
    trace_ = right->add(mk<TraceChart>(std::move(series), std::move(lanes), 150.0f));
    meter_ = right->add(mk<TorqueMeter>(26.0f), Pack{.top = 10});
    top->add(std::move(right));
    hero->body->add(std::move(top));
    page->add(std::move(hero), Pack{.bottom = 16});

    auto cols = mk<Columns>(std::vector<float>{1, 1}, 420.0f);
    auto col_a = mk<Box>(false);

    auto c1 = mk<Card>("Siła");
    bind_slider(c1->body, "Wzmocnienie globalne", "", 0.0, 1.0, &f.overall_gain, PCT, 0.01, d.overall_gain);
    bind_slider(c1->body, "Ograniczenie maksimum", "Chroni silnik i zasilacz - obcina szczytowy moment.", 0.05, 1.0, &f.max_force, PCT, 0.01, d.max_force);
    bind_slider(c1->body, "Siła minimalna", "Dodawana do każdego niezerowego momentu, żeby pokonać tarcie przekładni. Za duża wartość daje szarpanie.", 0.0, 0.5, &f.min_force, PCT, 0.01, d.min_force);
    bind_slider(c1->body, "Martwa strefa momentu", "", 0.0, 0.2, &f.deadband, PCT1, 0.005, d.deadband);
    bind_toggle(c1->body, "Odwróć kierunek działania silnika", "", &f.invert);
    col_a->add(std::move(c1), Pack{.bottom = 16});

    auto c2 = mk<Card>("Efekty generowane lokalnie", "Działają także w grach, które nie wysyłają FFB.");
    bind_slider(c2->body, "Sprężyna (samocentrowanie)", "", 0.0, 1.0, &f.spring_gain, PCT, 0.01, d.spring_gain);
    bind_slider(c2->body, "Tłumienie", "", 0.0, 1.0, &f.damper_gain, PCT, 0.01, d.damper_gain);
    bind_slider(c2->body, "Tarcie", "", 0.0, 1.0, &f.friction_gain, PCT, 0.01, d.friction_gain);
    col_a->add(std::move(c2));
    cols->add(std::move(col_a));

    auto col_b = mk<Box>(false);
    auto c3 = mk<Card>("Skalowanie efektów z gry");
    bind_slider(c3->body, "Siła stała / narastanie", "", 0.0, 2.0, &f.game_constant_gain, PCT, 0.05, d.game_constant_gain);
    bind_slider(c3->body, "Efekty okresowe (wibracje)", "", 0.0, 2.0, &f.game_periodic_gain, PCT, 0.05, d.game_periodic_gain);
    bind_slider(c3->body, "Sprężyna z gry", "", 0.0, 2.0, &f.game_spring_gain, PCT, 0.05, d.game_spring_gain);
    bind_slider(c3->body, "Tłumienie i tarcie z gry", "", 0.0, 2.0, &f.game_damper_gain, PCT, 0.05, d.game_damper_gain);
    std::vector<std::string> dir_labels;
    for (const auto& m : DIRECTION_MODES) dir_labels.push_back(DIRECTION_LABELS.at(m));
    auto dir = mk<Combo>(dir_labels, index_of(DIRECTION_MODES, f.direction_mode),
                         [this, dir_labels](int i) {
                             // w konfiguracji - wczesniej wybor przepadal przy restarcie
                             app_.cfg().ffb.direction_mode = DIRECTION_MODES[static_cast<size_t>(i)];
                             app_.engine().publish_config();
                             app_.log("info", "Tryb kierunku FFB: " + dir_labels[static_cast<size_t>(i)]);
                         },
                         26);
    direction_ = dir.get();
    add_row(c3->body, "Interpretacja kierunku", "Gry różnie kodują kierunek siły. Jeśli FFB działa odwrotnie albo tylko w jedną stronę, zmień ten tryb.", std::move(dir));
    col_b->add(std::move(c3), Pack{.bottom = 16});

    auto c4 = mk<Card>("Mostek H i bezpieczeństwo");
    bind_slider_int(c4->body, "PWM minimalne", "", 0, 254, &f.min_duty, PLAIN, 1, d.min_duty);
    bind_slider_int(c4->body, "PWM maksymalne", "", 1, 255, &f.max_duty, PLAIN, 1, d.max_duty);
    bind_slider_int(c4->body, "Max zmiana momentu na ms", "Ogranicza szarpnięcia. 65280 praktycznie wyłącza limit.", 256, 65280, &f.slew_per_ms, PLAIN, 256, d.slew_per_ms);
    bind_slider_int(c4->body, "Watchdog płytki", "Po tym czasie bez komendy z PC firmware zwalnia silnik.", 20, 1000, &f.timeout_ms, Unit{0, " ms", 1.0}, 10, d.timeout_ms);
    bind_slider_int(c4->body, "Częstotliwość FFB", "", 50, 1000, &f.update_hz, Unit{0, " Hz", 1.0}, 10, d.update_hz);
    col_b->add(std::move(c4));
    cols->add(std::move(col_b));

    page->add(std::move(cols), Pack{.bottom = 8});
    return page;
}

std::string FfbPage::state_text(const Frame& f) const {
    const AppConfig& cfg = app_.cfg();
    if (!cfg.ffb.enabled) return "Wyłączone - silnik jest zwolniony.";
    std::string txt;
    const FfbManager& ffb = app_.engine().ffb();
    if (ffb.registered()) {
        txt = std::format("Aktywne. Pakietów z gry: {}, wzmocnienie urządzenia: {:.0f} %", ffb.packets.load(), ffb.device_gain() * 100);
        if (ffb.parse_errors.load()) txt += std::format(", odrzucone pakiety: {}", ffb.parse_errors.load());
    } else {
        txt = "Włączone, ale bez odbioru z gry - działają tylko efekty lokalne (sprężyna, tłumienie, tarcie).";
    }
    if (!f.st.board_has_ffb && f.connected) txt += "  UWAGA: firmware zgłosił brak obsługi FFB.";
    return txt;
}

void FfbPage::refresh(const Frame& f, bool visible) {
    if (f.live) {
        std::array<float, TraceChart::MAX_SERIES> values{};
        values[0] = static_cast<float>(f.st.ffb_torque);
        values[1] = static_cast<float>(std::max(-1.0, std::min(1.0, f.state->steer_axis)));
        trace_->push(f.now, values, visible);
    }
    if (!visible) return;
    trace_->set_live(f.live);
    sync();
    AppConfig& cfg = app_.cfg();
    on_->set(cfg.ffb.enabled);
    title_->set(cfg.ffb.enabled ? "Silnik aktywny" : "Silnik zwolniony", cfg.ffb.enabled ? GOOD : TEXT);
    direction_->set_index(index_of(DIRECTION_MODES, cfg.ffb.direction_mode));
    meter_->set(f.st.ffb_torque);
    state_->set(state_text(f));
    std::vector<std::string> active;
    if (cfg.ffb.enabled && app_.engine().ffb().registered())
        for (const auto& [index, name, magnitude] : app_.engine().ffb().active_effects()) active.push_back(name);
    effects_->set(active.empty() ? "" : "Aktywne efekty: " + join(active, ", "));
}

} // namespace wb::ui

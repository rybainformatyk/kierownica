// Zakladki przypisan: kierownica G29 i pad Xbox.
#include <algorithm>
#include <cmath>
#include <format>

#include "app.h"
#include "../util.h"

namespace wb::ui {

namespace {

constexpr Unit DEG{0, "°", 1.0};
constexpr Unit TWO{2, "", 1.0};
constexpr Unit PCT{0, "%", 100.0};

const Names PAD_WIZARD_ORDER = {"A", "B", "X", "Y", "LB", "RB", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "START", "BACK", "LS", "RS", "GUIDE"};

std::string g29_label(const std::string& name) {
    auto it = G29_CONTROL_LABELS.find(name);
    return it == G29_CONTROL_LABELS.end() ? name : it->second;
}

} // namespace

// ----------------------------------------------------------------- G29Page

AppConfig& G29Page::cfg() { return app_.cfg(); }
void G29Page::sync_view() { view_->set_assignments(cfg().g29.buttons); }

int G29Page::filled() {
    int n = 0;
    for (const auto& [name, value] : cfg().g29.buttons)
        if (value >= 0) ++n;
    return n;
}

std::unique_ptr<Widget> G29Page::build() {
    auto page = mk<Box>(false);
    auto card = mk<Card>("Przyciski kierownicy G29");

    auto tools = mk<Box>(true);
    // plakietka stanu otwiera od razu wybor trybu - jak dawny combobox na tej zakladce
    state_ = tools->add(mk<Chip>("", MUTED, [this] { app_.open_mode_popover(state_->r); }, true), Pack{.right = 12, .cross = Pack::Center});
    auto wizard = mk<Button>("Przypisz po kolei", Button::Accent, [this] { wizard_start(); });
    wizard->set_icon(ic::play);
    tools->add(std::move(wizard), Pack{.cross = Pack::Center});
    tools->add(mk<Button>("Przepisz z pada", Button::Small, [this] { copy_from_pad(); }), Pack{.left = 6, .cross = Pack::Center});
    tools->add(mk<Button>("Domyślne", Button::Small, [this] { defaults(); }), Pack{.left = 6, .cross = Pack::Center});
    tools->add(mk<Button>("Wyczyść wszystko", Button::Small, [this] { clear_all(); }), Pack{.left = 6, .cross = Pack::Center});
    tools_ = card->right_slot->add(std::move(tools));

    auto wiz = mk<Box>(true);
    wiz->add(mk<Button>("Wstecz", Button::Small, [this] { if (!wizard_.empty() && wizard_i_ > 0) wizard_advance(-1); }), Pack{.cross = Pack::Center});
    wiz->add(mk<Button>("Pomiń", Button::Small, [this] { wizard_advance(+1); }), Pack{.left = 6, .cross = Pack::Center});
    wiz->add(mk<Button>("Wyczyść i pomiń", Button::Small, [this] { wizard_clear(); }), Pack{.left = 6, .cross = Pack::Center});
    wiz->add(mk<Button>("Zakończ", Button::Accent, [this] { wizard_stop(); }), Pack{.left = 6, .cross = Pack::Center});
    wiz->visible = false;
    wizard_tools_ = card->right_slot->add(std::move(wiz));

    live_ = card->body->add(mk<Label>("", Font::Small, GOOD), Pack{.bottom = 10, .cross = Pack::Start});

    PadView::Callbacks cb;
    cb.assign_request = [this](const std::string& c) { on_assign_request(c); };
    cb.clear = [this](const std::string& c) { on_clear(c); };
    auto view = make_wheel_view(std::move(cb));
    view_ = view.get();
    card->body->add(std::move(view));
    view_->set_assignments(cfg().g29.buttons);
    card->body->add(note("Te przypisania działają w trybach z G29 (Kierownica G29 oraz G29 + vJoy) i nie zależą od zakładki Pad ani od listy vJoy w zakładce Przyciski. "
                         "Krzyżak bez przypisań bierze kierunki z krzyżaka ustawionego w zakładce Przyciski. Biegi to przyciski skrzyni G29 - przypisz je, jeśli masz własną skrzynię albo manetki.", 1400.0f),
                    Pack{.top = 12});
    page->add(std::move(card), Pack{.bottom = 8});
    return page;
}

void G29Page::notify(const std::string& text, double seconds) {
    notice_until_ = monotonic() + seconds;
    live_->set(text, GOOD);
}

void G29Page::refresh(const Frame& f, bool visible) {
    if (f.state && f.connected) capture(f.state->buttons_effective);
    view_->tick();
    if (!visible) return;

    // stan G29 jednym slowem w naglowku karty
    std::string state = app_.g29_state(f.connected, f.st);
    if (state == "offline") state_->set("Brak połączenia z płytką", MUTED);
    else if (state == "old_fw") state_->set("Firmware bez trybu G29", BAD);
    else if (state == "no_board") state_->set("Płytka bez G29 - użyj vJoy lub pada", BAD);
    else if (!cfg().output.g29_on()) state_->set("Tryb G29 wyłączony", WARN);
    else if (state == "attaching" || state == "detecting") state_->set("G29 podłącza się…", WARN);
    else if (state == "no_usb") state_->set(f.st.g29_same_cable ? "Windows nie widzi G29" : "Podłącz Native USB", BAD);
    else state_->set("Windows widzi G29", GOOD);
    sync_view();

    auto* wheel = static_cast<WheelCanvas*>(view_->canvas());
    if (!f.state || !f.connected) {
        view_->set_active({});
        wheel->set_angle(0.0);
        wheel->set_pedals({});
        set_live(std::nullopt);
        return;
    }
    // pokazujemy to, co faktycznie idzie do gry; poza trybem G29 - to, co by poszlo
    auto report = cfg().output.g29_on() ? app_.engine().last_g29() : std::nullopt;
    auto active = g29_active(report ? *report : build_g29_report(*f.state, cfg()));
    view_->set_active(active);
    wheel->set_angle(f.state->steer_deg);
    wheel->set_pedals(f.state->pedals);
    set_live(active);
}

void G29Page::set_live(const std::optional<std::map<std::string, bool>>& active) {
    if (monotonic() < notice_until_) return;  // swiezy komunikat ma pierwszenstwo
    const auto& learning = view_->learning();
    std::string text;
    RGB color = GOOD;
    if (learning && wizard_.empty()) {
        text = "Czekam na przycisk kierownicy dla: " + g29_label(*learning);
    } else if (learning) {
        text = "";  // w kreatorze krok widac w podpowiedzi nad rysunkiem
    } else if (!active) {
        text = OFFLINE_LIVE;
        color = MUTED;
    } else {
        std::vector<std::string> names;
        for (const auto& n : G29_CONTROLS) {
            auto it = active->find(n);
            if (it != active->end() && it->second) names.push_back(g29_label(n));
        }
        text = "Wciśnięte na G29: " + (names.empty() ? std::string("nic") : join(names, ", "));
    }
    live_->set(text, color);
}

void G29Page::on_assign_request(const std::string&) {
    app_.go_tab(TAB_G29);
    // druga zakladka nie moze uczyc sie w tym samym czasie
    app_.pad_page()->stop_learning();
    // przycisk trzymany w chwili klikniecia nie moze od razu wpasc do tej kontrolki
    auto state = app_.engine().last_state();
    prev_buttons_ = state ? state->buttons_effective : 0;
    notice_until_ = 0;
}

void G29Page::on_clear(const std::string& control) {
    cfg().g29.buttons[control] = -1;
    cfg().normalize();
    sync_view();
    app_.log("info", "G29: wyczyszczono " + g29_label(control));
}

void G29Page::capture(uint32_t mask) {
    auto control = view_->learning();
    if (!control) {
        prev_buttons_ = mask;
        return;
    }
    std::vector<int> held;
    for (int i = 0; i < protocol::NUM_BUTTONS; ++i)
        if ((mask >> i) & 1) held.push_back(i);
    view_->set_held_buttons(held);

    uint32_t fresh = mask & ~prev_buttons_;
    prev_buttons_ = mask;
    if (!fresh) return;
    int index = 0;
    while (!((fresh >> index) & 1)) ++index;

    ButtonMap& buttons = cfg().g29.buttons;
    std::vector<std::string> stolen;
    for (auto& [name, value] : buttons)
        if (value == index && name != *control) {
            value = -1;
            stolen.push_back(g29_label(name));
        }
    buttons[*control] = index;
    cfg().normalize();
    sync_view();

    std::string note_text = stolen.empty() ? "" : "  (zabrany z: " + join(stolen, ", ") + ")";
    notify(std::format("Przypisano przycisk {} do: {}{}", index, g29_label(*control), note_text));
    app_.log("ok", std::format("G29: przycisk {} -> {}{}", index, g29_label(*control), note_text));

    if (!wizard_.empty()) wizard_advance(+1);
    else view_->cancel_learning();
}

void G29Page::escape() {
    if (!wizard_.empty()) wizard_stop();
    else view_->cancel_learning();
}

void G29Page::stop() {
    if (!wizard_.empty()) wizard_stop();
    else if (view_->learning()) view_->cancel_learning();
}

void G29Page::copy_from_pad() {
    stop();
    ButtonMap& buttons = cfg().g29.buttons;
    ButtonMap from_pad = g29_buttons_from_pad(cfg().pad.buttons);
    std::vector<int> taken;
    Names targets;
    for (const auto& [pad, g29_name] : PAD_TO_G29) {
        targets.push_back(g29_name);
        buttons[g29_name] = from_pad[g29_name];
        if (from_pad[g29_name] >= 0) taken.push_back(from_pad[g29_name]);
    }
    for (const auto& name : G29_CONTROLS)
        if (!contains(targets, name) && std::find(taken.begin(), taken.end(), buttons[name]) != taken.end()) buttons[name] = -1;
    cfg().normalize();
    sync_view();
    notify(std::format("Przepisano przypisania z pada - przypisanych kontrolek: {} z {}", filled(), G29_CONTROLS.size()));
    app_.log("ok", "G29: przepisano przypisania z zakładki Pad");
}

void G29Page::defaults() {
    stop();
    cfg().g29.buttons = g29_buttons_from_pad(DEFAULT_PAD_BUTTONS);
    cfg().normalize();
    sync_view();
    app_.log("ok", "G29: przywrócono domyślne przypisania");
}

void G29Page::clear_all() {
    stop();
    ButtonMap empty;
    for (const auto& name : G29_CONTROLS) empty[name] = -1;
    cfg().g29.buttons = empty;
    cfg().normalize();
    sync_view();
    app_.log("info", "G29: wyczyszczono wszystkie przypisania");
}

void G29Page::wizard_start() {
    app_.go_tab(TAB_G29);
    wizard_ = G29_WIZARD_ORDER;
    wizard_i_ = 0;
    tools_->set_visible(false);
    wizard_tools_->set_visible(true);
    app_.log("info", "G29: kreator przypisań - wciskaj kolejne przyciski kierownicy, Esc kończy");
    wizard_show();
}

void G29Page::wizard_show() {
    if (wizard_.empty()) return;
    if (wizard_i_ < 0 || wizard_i_ >= static_cast<int>(wizard_.size())) {
        wizard_stop(true);
        return;
    }
    view_->set_wizard(wizard_i_ + 1, static_cast<int>(wizard_.size()));
    view_->start_learning(wizard_[static_cast<size_t>(wizard_i_)]);
}

void G29Page::wizard_advance(int delta) {
    if (wizard_.empty()) return;
    wizard_i_ += delta;
    wizard_show();
}

void G29Page::wizard_clear() {
    if (wizard_.empty()) return;
    cfg().g29.buttons[wizard_[static_cast<size_t>(wizard_i_)]] = -1;
    cfg().normalize();
    sync_view();
    wizard_advance(+1);
}

void G29Page::wizard_stop(bool done) {
    if (wizard_.empty()) return;
    wizard_.clear();
    view_->set_wizard(std::nullopt, std::nullopt);
    view_->cancel_learning();
    wizard_tools_->set_visible(false);
    tools_->set_visible(true);
    int n = filled();
    notify(std::format("Kreator zakończony - przypisanych kontrolek: {} z {}", n, G29_CONTROLS.size()), 6.0);
    app_.log(done ? "ok" : "info", std::format("G29: kreator zakończony ({} z {} kontrolek przypisanych)", n, G29_CONTROLS.size()));
}

// ----------------------------------------------------------------- PadPage

std::unique_ptr<Widget> PadPage::build() {
    AppConfig& cfg = app_.cfg();
    const PadConfig d;
    auto page = mk<Box>(false);

    auto view = mk<Card>("Przypisania");
    auto tools = mk<Box>(true);
    // plakietka stanu otwiera od razu wybor trybu - jak dawny combobox na tej zakladce
    state_ = tools->add(mk<Chip>("", MUTED, [this] { app_.open_mode_popover(state_->r); }, true), Pack{.right = 12, .cross = Pack::Center});
    auto wizard = mk<Button>("Przypisz po kolei", Button::Accent, [this] { wizard_start(); });
    wizard->set_icon(ic::play);
    tools->add(std::move(wizard), Pack{.cross = Pack::Center});
    tools->add(mk<Button>("Domyślne", Button::Small, [this] { defaults(); }), Pack{.left = 6, .cross = Pack::Center});
    tools->add(mk<Button>("Wyczyść wszystko", Button::Small, [this] { clear_all(); }), Pack{.left = 6, .cross = Pack::Center});
    tools_ = view->right_slot->add(std::move(tools));

    auto wiz = mk<Box>(true);
    wiz->add(mk<Button>("Wstecz", Button::Small, [this] { if (!wizard_.empty() && wizard_i_ > 0) wizard_advance(-1); }), Pack{.cross = Pack::Center});
    wiz->add(mk<Button>("Pomiń", Button::Small, [this] { wizard_advance(+1); }), Pack{.left = 6, .cross = Pack::Center});
    wiz->add(mk<Button>("Wyczyść i pomiń", Button::Small, [this] { wizard_clear(); }), Pack{.left = 6, .cross = Pack::Center});
    wiz->add(mk<Button>("Zakończ", Button::Accent, [this] { wizard_stop(); }), Pack{.left = 6, .cross = Pack::Center});
    wiz->visible = false;
    wizard_tools_ = view->right_slot->add(std::move(wiz));

    live_ = view->body->add(mk<Label>("", Font::Small, GOOD), Pack{.bottom = 10, .cross = Pack::Start});
    conflict_ = view->body->add(mk<Label>("", Font::Small, WARN, 2000.0f), Pack{.bottom = 8});
    conflict_->visible = false;

    PadView::Callbacks cb;
    cb.assign_request = [this](const std::string& c) { on_assign_request(c); };
    cb.clear = [this](const std::string& c) { on_clear(c); };
    cb.axis_click = [this](const std::string& dir, float x, float y) { on_axis_click(dir, x, y); };
    cb.axis_clear = [this](const std::string& dir) { set_axis_source(dir, ""); };
    cb.cancel = [this] { if (!wizard_.empty()) wizard_stop(); };
    auto pad = make_pad_view(std::move(cb));
    view_ = pad.get();
    view->body->add(std::move(pad));
    view_->set_assignments(cfg.pad.buttons);
    page->add(std::move(view), Pack{.bottom = 16});

    // --- osie ---
    auto cols = mk<Columns>(std::vector<float>{1, 1}, 420.0f);
    auto axes = mk<Card>("Skręt na drążku");
    auto after = [this] { on_field_changed(); };
    bind_slider_int(axes->body, "Zakres kierownicy na pełne wychylenie", "Pad ma mały zakres drążka. Dla gier zręcznościowych 270-450 stopni daje naturalne prowadzenie; pełne 900 sprawi, że auto ledwo skręca.",
                    45, 1080, &cfg.pad.steer_range_deg, DEG, 15, d.steer_range_deg, after);
    bind_slider(axes->body, "Martwa strefa wokół środka", "Własna martwa strefa aplikacji. Zostaw 0, jeśli auto nie drga, gdy trzymasz kierownicę na środku.", 0.0, 0.40, &cfg.pad.steer_deadzone, PCT, 0.01, d.steer_deadzone, after);
    bind_slider(axes->body, "Kompensacja martwej strefy gry", "Na tym choruje Forza: gra sama ucina początek wychylenia drążka, więc pierwsze stopnie obrotu kierownicy nic nie robią. Tu najmniejszy ruch od razu przebija próg gry. Najpierw ustaw w grze martwą strefę wewnętrzną skrętu na 0, potem podnoś ten suwak, aż auto reaguje natychmiast.",
                0.0, 0.60, &cfg.pad.steer_antideadzone, PCT, 0.01, d.steer_antideadzone, after);
    bind_slider(axes->body, "Krzywa skrętu", "1.00 = liniowo. Poniżej 1 środek jest czulszy, powyżej 1 spokojniejszy przy środku, a agresywny na końcach.", 0.3, 3.0, &cfg.pad.steer_linearity, TWO, 0.05, d.steer_linearity, after);
    bind_toggle(axes->body, "Odwróć kierunek skrętu na padzie", "", &cfg.pad.steer_invert);
    cols->add(std::move(axes));

    auto right = mk<Card>("Profil i spusty");
    std::vector<std::string> profile_labels;
    for (const auto& k : PAD_PROFILE_ORDER) profile_labels.push_back(PAD_PROFILE_LABELS.at(k));
    auto combo = mk<Combo>(profile_labels, index_of(PAD_PROFILE_ORDER, cfg.pad.detect_profile()), [this](int i) { on_profile(i); }, 24);
    profile_ = combo.get();
    add_row(right->body, "Profil gry", "Ustawia tylko skalowanie skrętu i pedałów pod daną grę. Przypisania przycisków i kierunków zostają nietknięte.", std::move(combo));
    bind_slider(right->body, "Kompensacja martwej strefy spustów", "To samo dla gazu i hamulca na LT/RT - przydaje się, gdy gra ignoruje pierwsze procenty wciśnięcia pedału.", 0.0, 0.60, &cfg.pad.trigger_antideadzone, PCT, 0.01, d.trigger_antideadzone, after);
    right->body->add(note("Spusty Xboxa mają 8 bitów (0-255), więc pedały są tu mniej precyzyjne niż w trybie kierownicy. To ograniczenie XInputa, nie aplikacji."), Pack{.top = 10});
    right->body->add(note("Forza, NFS Unbound i inne gry z obsługą kierownic najlepiej działają w trybie Kierownica G29 - widzą wtedy prawdziwą kierownicę i ustawienia pada nie są potrzebne. Tryb pada zostaw dla gier bez kierownic."), Pack{.top = 8});
    cols->add(std::move(right));
    page->add(std::move(cols), Pack{.bottom = 8});
    return page;
}

void PadPage::on_field_changed() {
    app_.cfg().normalize();
    profile_->set_index(index_of(PAD_PROFILE_ORDER, app_.cfg().pad.detect_profile()));
}

void PadPage::on_profile(int index) {
    const std::string& key = PAD_PROFILE_ORDER[static_cast<size_t>(index)];
    AppConfig& cfg = app_.cfg();
    if (key.empty()) {
        // Wlasny nie jest zestawem wartosci - nazwa musi wrocic do tego, co
        // naprawde jest na suwakach.
        profile_->set_index(index_of(PAD_PROFILE_ORDER, cfg.pad.detect_profile()));
        return;
    }
    cfg.pad.apply_profile(key);
    cfg.normalize();
    app_.log("info", "Profil pada: " + PAD_PROFILE_LABELS.at(key));
    Ui::get().toast("Profil pada: " + PAD_PROFILE_LABELS.at(key));
}

void PadPage::notify(const std::string& text, double seconds) {
    notice_until_ = monotonic() + seconds;
    live_->set(text, GOOD);
}

bool PadPage::source_available(const std::string& source) const {
    int idx = pedal_index(source);
    return idx < 0 || app_.cfg().pedals[static_cast<size_t>(idx)].source >= 0;
}

std::string PadPage::axis_label(const std::string& direction) const {
    std::string source = app_.cfg().pad.source_of(direction);
    if (source.empty()) return "";
    std::string label = PAD_SOURCE_LABELS.at(source);
    // komorka jest waska - "Kierownica w lewo" nie miescila sie w calosci
    if (label.starts_with("Kierownica")) label = "Skręt" + label.substr(10);
    return source_available(source) ? label : label + " (!)";
}

void PadPage::sync_axis_labels() {
    LabelMap labels;
    for (const auto& d : PAD_DIRECTIONS) labels[d] = axis_label(d);
    view_->set_axis_labels(labels);
}

void PadPage::set_axis_source(const std::string& direction, const std::string& source) {
    AppConfig& cfg = app_.cfg();
    cfg.pad.axes[direction] = source;
    cfg.normalize();
    sync_axis_labels();
    const std::string& name = PAD_DIRECTION_LABELS.at(direction);
    if (!source.empty()) app_.log("ok", "Pad: " + PAD_SOURCE_LABELS.at(source) + " -> " + name);
    else app_.log("info", "Pad: wyczyszczono " + name);
}

void PadPage::on_axis_click(const std::string& direction, float x, float y) {
    std::string current = app_.cfg().pad.source_of(direction);
    PopupMenu menu(upper_pl(PAD_DIRECTION_LABELS.at(direction)));
    for (const auto& key : PAD_SOURCE_KEYS)
        menu.add(PAD_SOURCE_LABELS.at(key), [this, direction, key] { set_axis_source(direction, key); }, key == current,
                 source_available(key) ? "" : "kanał wyłączony");
    menu.add_separator();
    menu.add("Nie używaj", [this, direction] { set_axis_source(direction, ""); }, current.empty());
    menu.popup(x, y);
}

void PadPage::stop_learning() {
    if (!wizard_.empty()) wizard_stop();
    else view_->cancel_learning();
}

void PadPage::on_assign_request(const std::string& control) {
    app_.go_tab(TAB_PAD);
    app_.g29_page()->stop();
    if (!wizard_.empty()) {
        // Klik w inna kontrolke w trakcie kreatora: kreator idzie za nim.
        if (contains(wizard_, control) && wizard_[static_cast<size_t>(wizard_i_)] != control) {
            wizard_i_ = index_of(wizard_, control);
            view_->set_wizard(wizard_i_ + 1, static_cast<int>(wizard_.size()));
        }
        return;
    }
    notify("Czekam na przycisk kierownicy dla: " + PAD_BUTTON_LABELS.at(control));
}

void PadPage::on_clear(const std::string& control) {
    AppConfig& cfg = app_.cfg();
    cfg.pad.buttons[control] = -1;
    cfg.normalize();
    view_->set_assignments(cfg.pad.buttons);
    app_.log("info", "Wyczyszczono przypisanie: " + PAD_BUTTON_LABELS.at(control));
}

void PadPage::defaults() {
    AppConfig& cfg = app_.cfg();
    cfg.pad.buttons = DEFAULT_PAD_BUTTONS;
    cfg.pad.axes = DEFAULT_PAD_AXES;
    cfg.normalize();
    view_->set_assignments(cfg.pad.buttons);
    sync_axis_labels();
    app_.log("ok", "Pad: przywrócono domyślne przypisania (skręt na lewym drążku, gaz na RT, hamulec na LT)");
}

void PadPage::clear_all() {
    AppConfig& cfg = app_.cfg();
    for (const auto& n : PAD_BUTTON_NAMES) cfg.pad.buttons[n] = -1;
    for (const auto& d : PAD_DIRECTIONS) cfg.pad.axes[d] = "";
    cfg.normalize();
    view_->set_assignments(cfg.pad.buttons);
    sync_axis_labels();
    app_.log("info", "Wyczyszczono wszystkie przypisania pada");
}

void PadPage::capture(uint32_t mask) {
    auto control = view_->learning();
    if (!control) {
        prev_buttons_ = mask;
        return;
    }
    // podglad na zywo - widac, ze plytka w ogole cos wysyla
    std::vector<int> held;
    for (int i = 0; i < protocol::NUM_BUTTONS; ++i)
        if ((mask >> i) & 1) held.push_back(i);
    view_->set_held_buttons(held);

    uint32_t fresh = mask & ~prev_buttons_;
    prev_buttons_ = mask;
    if (!fresh) return;
    int index = 0;
    while (!((fresh >> index) & 1)) ++index;

    AppConfig& cfg = app_.cfg();
    // ten sam przycisk nie moze sterowac dwiema kontrolkami naraz
    std::vector<std::string> stolen;
    for (auto& [name, assigned] : cfg.pad.buttons)
        if (assigned == index && name != *control) {
            assigned = -1;
            stolen.push_back(PAD_BUTTON_LABELS.at(name));
        }
    cfg.pad.buttons[*control] = index;
    cfg.normalize();
    view_->set_assignments(cfg.pad.buttons);

    const std::string& label = PAD_BUTTON_LABELS.at(*control);
    std::string note_text = stolen.empty() ? "" : "  (zabrany z: " + join(stolen, ", ") + ")";
    notify(std::format("Przypisano przycisk {} do {}{}", index, label, note_text));
    app_.log("ok", std::format("Pad: przycisk {} -> {}{}", index, label, note_text));

    if (!wizard_.empty()) wizard_advance(+1);
    else view_->cancel_learning();
}

void PadPage::wizard_start() {
    app_.go_tab(TAB_PAD);
    wizard_ = PAD_WIZARD_ORDER;
    wizard_i_ = 0;
    tools_->set_visible(false);
    wizard_tools_->set_visible(true);
    app_.log("info", "Pad: kreator przypisań - wciskaj kolejne przyciski kierownicy, Esc kończy");
    wizard_show();
}

void PadPage::wizard_show() {
    if (wizard_.empty()) return;
    if (wizard_i_ < 0 || wizard_i_ >= static_cast<int>(wizard_.size())) {
        wizard_stop(true);
        return;
    }
    view_->set_wizard(wizard_i_ + 1, static_cast<int>(wizard_.size()));
    view_->start_learning(wizard_[static_cast<size_t>(wizard_i_)]);
    // zeby przycisk trzymany z poprzedniego kroku nie wpadl od razu do nastepnej
    auto state = app_.engine().last_state();
    prev_buttons_ = state ? state->buttons_effective : 0;
}

void PadPage::wizard_advance(int delta) {
    if (wizard_.empty()) return;
    wizard_i_ += delta;
    wizard_show();
}

void PadPage::wizard_clear() {
    if (wizard_.empty()) return;
    AppConfig& cfg = app_.cfg();
    cfg.pad.buttons[wizard_[static_cast<size_t>(wizard_i_)]] = -1;
    cfg.normalize();
    view_->set_assignments(cfg.pad.buttons);
    wizard_advance(+1);
}

void PadPage::wizard_stop(bool done) {
    if (wizard_.empty()) return;
    wizard_.clear();
    view_->set_wizard(std::nullopt, std::nullopt);
    view_->cancel_learning();
    wizard_tools_->set_visible(false);
    tools_->set_visible(true);
    AppConfig& cfg = app_.cfg();
    int filled = 0;
    for (const auto& n : PAD_BUTTON_NAMES)
        if (cfg.pad.buttons.at(n) >= 0) ++filled;
    notify(std::format("Kreator zakończony - przypisanych kontrolek: {} z {}", filled, PAD_BUTTON_NAMES.size()), 6.0);
    app_.log(done ? "ok" : "info", std::format("Pad: kreator zakończony ({} z {} kontrolek przypisanych)", filled, PAD_BUTTON_NAMES.size()));
}

std::vector<std::string> PadPage::axis_warnings() const {
    const PadConfig& pad = app_.cfg().pad;
    std::vector<std::string> out;
    for (const auto& d : PAD_DIRECTIONS) {
        std::string source = pad.source_of(d);
        if (!source.empty() && !source_available(source))
            out.push_back(PAD_DIRECTION_LABELS.at(d) + ": " + PAD_SOURCE_LABELS.at(source) + " ma wyłączony kanał - ten kierunek nic nie wysyła");
    }
    // to samo zrodlo na obu polowkach jednej osi znosi sie do zera
    for (const char* axis : {"LX", "LY", "RX", "RY"}) {
        std::string minus = pad.source_of(std::string(axis) + "-");
        std::string plus = pad.source_of(std::string(axis) + "+");
        if (!minus.empty() && minus == plus)
            out.push_back(std::format("{} steruje oboma kierunkami osi {} naraz - wychylenia znoszą się do zera", PAD_SOURCE_LABELS.at(minus), axis));
    }
    if (pad.source_of("LX-").empty() && pad.source_of("LX+").empty())
        out.push_back("Lewy drążek nie ma wpiętego skrętu - w grze auto nie będzie skręcać");
    return out;
}

void PadPage::refresh(const Frame& f, bool visible) {
    if (f.live) capture(f.state->buttons_effective);
    view_->tick();
    if (!visible) return;
    sync();
    AppConfig& cfg = app_.cfg();
    profile_->set_index(index_of(PAD_PROFILE_ORDER, cfg.pad.detect_profile()));
    view_->set_assignments(cfg.pad.buttons);
    sync_axis_labels();

    // bez lacza nie pokazujemy ostatniej ramki - pad wyglada wtedy, jakby
    // przycisk dalej byl wcisniety
    std::optional<PadState> pad_state;
    if (f.live) {
        pad_state = app_.engine().last_pad();
        if (!pad_state) pad_state = build_pad_state(*f.state, cfg);
    }
    bool idle = !view_->learning() && monotonic() >= notice_until_;
    if (pad_state) {
        std::map<std::string, bool> active;
        std::vector<std::string> names;
        for (const auto& n : PAD_BUTTON_NAMES) {
            bool on = (pad_state->buttons & xusb_mask(n)) != 0;
            active[n] = on;
            if (on) names.push_back(PAD_BUTTON_LABELS.at(n));
        }
        view_->set_active(active);
        view_->set_triggers(pad_state->lt, pad_state->rt);
        view_->set_sticks(pad_state->lx / 32767.0, pad_state->ly / 32767.0, pad_state->rx / 32767.0, pad_state->ry / 32767.0);
        if (idle) live_->set("Aktywne na padzie: " + (names.empty() ? std::string("nic") : join(names, ", ")), GOOD);
    } else {
        view_->set_active({});
        view_->set_triggers(0, 0);
        view_->set_sticks(0, 0, 0, 0);
        if (idle) live_->set(OFFLINE_LIVE, MUTED);
    }

    auto warnings = axis_warnings();
    // pelny opis bledu tutaj - plakietka u gory ma byc krotka
    std::string pad_err = cfg.output.pad_on() && !f.st.pad_ok ? app_.engine().pad_error() : std::string();
    if (!pad_err.empty()) warnings.insert(warnings.begin(), first_line(pad_err));
    if (!warnings.empty()) {
        std::string text;
        for (const auto& w : warnings) text += (text.empty() ? "• " : "\n• ") + w;
        conflict_->set(text);
    }
    conflict_->set_visible(!warnings.empty());

    if (cfg.output.pad_on()) {
        if (f.st.pad_ok) state_->set("Pad podłączony", GOOD);
        else state_->set("Pad nie wystartował", BAD);
    } else {
        state_->set("Tryb pada wyłączony", WARN);
    }
}

} // namespace wb::ui

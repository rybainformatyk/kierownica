#include "app.h"

#include <shellapi.h>
#include <timeapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>

#include "../json.h"
#include "../winsys.h"

namespace fs = std::filesystem;

namespace wb::ui {

namespace {

constexpr const char* VJOY_URL = "https://sourceforge.net/projects/vjoystick/";
constexpr const char* SAVE_HINT = "Ctrl+S";
constexpr const char* DIRTY_HINT = "● Niezapisane zmiany · Ctrl+S";
constexpr double SAVED_FLASH_S = 4.0;     // tyle widac "Zapisano" po kliknieciu
constexpr UINT UI_PERIOD_MS = 16;         // ok. 60 klatek na sekunde
constexpr UINT UI_IDLE_PERIOD_MS = 200;   // okno niewidoczne - podgladu i tak nie widac
constexpr double G29_DETECT_S = 5.0;      // tyle Windows ma na wyliczenie ponownie podlaczonej G29
constexpr double HIDDEN_CHECK_S = 0.25;   // co ile sprawdzamy, czy okno jest zasloniete
constexpr UINT_PTR TIMER_ID = 1;
constexpr int MIN_W = 940, MIN_H = 600;

const std::array<const char*, 8> TAB_NAMES = {"Pulpit", "Kierownica", "Pedały", "Przyciski", "G29", "Pad", "Force Feedback", "Diagnostyka"};
const std::vector<std::string> TAB_ICONS = {"\xEE\xB1\x8A", "\xEE\x9C\x93", "\xEE\xA7\xA9", "\xEE\xA2\xA9", "\xEE\xA0\x84", "\xEE\x9F\xBC", "\xEE\xA5\x85", "\xEE\xA7\x99"};
const std::array<int, 7> RANGE_PRESETS = {120, 270, 360, 540, 720, 900, 1080};
const std::array<RGB, NUM_PEDALS> PEDAL_COLORS = {GOOD, BAD, WARN, ACCENT, ACCENT, ACCENT};
const std::array<const char*, 4> HAT_DIRECTIONS = {"góra", "prawo", "dół", "lewo"};
const Names PAD_WIZARD_ORDER = {"A", "B", "X", "Y", "LB", "RB", "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT", "START", "BACK", "LS", "RS", "GUIDE"};

struct GameRow { const char* games; const char* mode; const char* why; };
const GameRow GAME_COMPAT[] = {
    {"Forza, NFS Unbound, F1, ETS2 - każda gra z kierownicą", "kierownica G29",
     "Płytka sama przedstawia się jako Logitech G29 (046D:C24F), więc gry z listą obsługiwanych kierownic rozpoznają ją bez żadnej konfiguracji."},
    {"Gdy 25 przycisków G29 nie wystarcza (button box)", "G29 + vJoy",
     "Jazda idzie przez G29, a dodatkowe przyciski przez vJoy (lista w zakładce Przyciski). W grze przypisz je z urządzenia vJoy."},
    {"Gry bez obsługi kierownic (porty z konsol)", "tryb pada",
     "Czytają wyłącznie XInput, czyli pada. Kierownicy nie zobaczą w żadnym trybie."},
    {"Symulatory z Force Feedback (AC, rFactor, BeamNG)", "kierownica vJoy",
     "G29 też w nich działa, ale Force Feedback z gry idzie tylko przez vJoy. Lokalne efekty (sprężyna, tłumienie) działają w każdym trybie."},
    {"Gry, które umieją jedno i drugie", "oba naraz",
     "Tryb \"Oba naraz\" wystawia vJoy i pada jednocześnie; w grze wybierasz, które urządzenie ma być aktywne."},
};

constexpr const char* G29_BUTTON_HELP =
    "Przyciski G29 przypisujesz w zakładce G29 - kliknij przycisk na rysunku kierownicy i wciśnij go na swojej. "
    "Te przypisania nie zależą od zakładki Pad ani od listy vJoy w zakładce Przyciski.";

fs::path firmware_script() {
    // panel/cpp/bin -> katalog repozytorium; szukamy w gore, bo program moze lezec gdzie indziej
    fs::path dir = exe_dir();
    for (int i = 0; i < 6; ++i) {
        fs::path candidate = dir / L"firmware" / L"kierownica" / L"wgraj_firmware.bat";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    return exe_dir().parent_path().parent_path().parent_path() / L"firmware" / L"kierownica" / L"wgraj_firmware.bat";
}

fs::path exe_path() { return exe_dir() / L"Kierownica.exe"; }

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string fmt_duration(double seconds) {
    int s = static_cast<int>(std::max(0.0, seconds));
    int h = s / 3600, m = (s % 3600) / 60;
    s %= 60;
    return h ? std::format("{}:{:02d}:{:02d}", h, m, s) : std::format("{}:{:02d}", m, s);
}

std::string time_stamp() {
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    return std::format("{:02d}:{:02d}:{:02d}", local.tm_hour, local.tm_min, local.tm_sec);
}

std::string hex_bytes(const uint8_t* data, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += std::format("{}{:02x}", i ? " " : "", data[i]);
    return out;
}

RGB level_color(const std::string& level) {
    if (level == "ok") return GOOD;
    if (level == "warn") return WARN;
    if (level == "error") return BAD;
    return TEXT_DIM;
}

int index_of(const Names& list, const std::string& value, int fallback = 0) {
    auto it = std::find(list.begin(), list.end(), value);
    return it == list.end() ? fallback : static_cast<int>(it - list.begin());
}

template <typename T, typename... A>
std::unique_ptr<T> mk(A&&... args) { return std::make_unique<T>(std::forward<A>(args)...); }

std::unique_ptr<Label> dim(std::string text) { return mk<Label>(std::move(text), Font::Small, TEXT_DIM); }
std::unique_ptr<Label> muted(std::string text, float cap = 0) { return mk<Label>(std::move(text), Font::Small, MUTED, cap); }

Slider::Format fmt_int(const char* suffix = "") {
    std::string s = suffix;
    return [s](double v) { return std::format("{:.0f}{}", v, s); };
}
Slider::Format fmt_two() { return [](double v) { return std::format("{:.2f}", v); }; }
Slider::Format fmt_pct(int decimals = 0) {
    return [decimals](double v) { return std::format("{:.{}f}%", v * 100, decimals); };
}

// Prostokat z tlem i ramka wokol jednego dziecka.
class RoundPanel final : public Widget {
public:
    RoundPanel(std::unique_ptr<Widget> child, RGB fill, float radius, float pad_x, float pad_y, float width = 0)
        : child_(std::move(child)), fill_(fill), radius_(radius), pad_x_(pad_x), pad_y_(pad_y), width_(width) {}

    Size measure(float avail_w) override {
        float w = width_ > 0 ? width_ - 2 * pad_x_ : avail_w - 2 * pad_x_;
        Size s = child_->measure(std::max(0.0f, w));
        return {width_ > 0 ? width_ : avail_w, s.h + 2 * pad_y_};
    }
    void arrange(const Rect& rc) override {
        r = rc;
        child_->arrange({rc.x + pad_x_, rc.y + pad_y_, std::max(0.0f, rc.w - 2 * pad_x_), std::max(0.0f, rc.h - 2 * pad_y_)});
    }
    void paint(Gfx& g) override {
        g.fill_rrect(r, radius_, fill_);
        g.stroke_rrect(r, radius_, BORDER);
        child_->paint(g);
    }
    Widget* hit_test(float x, float y) override { return child_->hit_test(x, y); }

private:
    std::unique_ptr<Widget> child_;
    RGB fill_;
    float radius_, pad_x_, pad_y_, width_;
};

// Logo: obrecz, poziomy drazek i kolumna w dol.
class Logo final : public Widget {
public:
    Size measure(float) override { return {30, 30}; }
    Widget* hit_test(float, float) override { return nullptr; }
    void paint(Gfx& g) override {
        float x = r.x, y = r.y;
        g.stroke_circle(x + 15, y + 15, 13, ACCENT, 3);
        g.polyline({{x + 5, y + 15}, {x + 25, y + 15}}, ACCENT, 3);
        g.polyline({{x + 15, y + 15}, {x + 15, y + 26}}, ACCENT, 3);
        g.fill_circle(x + 15, y + 15, 4, PANEL);
        g.stroke_circle(x + 15, y + 15, 4, ACCENT, 2);
    }
};

// Etykieta z wlasnym tlem - numer wersji w naglowku.
class Badge final : public Widget {
public:
    explicit Badge(std::string text) : text_(std::move(text)) {}
    Size measure(float) override {
        auto s = Gfx::get().measure(text_, Font::TinyBold);
        return {s.width + 14, s.height + 4};
    }
    Widget* hit_test(float, float) override { return nullptr; }
    void paint(Gfx& g) override {
        g.fill_rect(r, PANEL_HI);
        g.text(r, text_, Font::TinyBold, TEXT_DIM, Align::Center);
    }

private:
    std::string text_;
};

// Wiersz jednego przycisku w zakladce Przyciski.
class Tick final : public Widget {
public:
    Tick(RGB color, float w, float h) : color_(color), w_(w), h_(h) {}
    Size measure(float) override { return {w_, h_}; }
    Widget* hit_test(float, float) override { return nullptr; }
    void paint(Gfx& g) override { g.fill_rect({r.x, r.y + (r.h - h_) / 2, w_, h_}, color_); }

private:
    RGB color_;
    float w_, h_;
};

} // namespace

// ------------------------------------------------------------------ G29Tab

class G29Tab {
public:
    explicit G29Tab(PanelApp& app) : app_(app) {}

    PadView* view = nullptr;
    Label* lbl_state = nullptr;
    Label* lbl_live = nullptr;
    Box* tools = nullptr;
    Box* wizard_tools = nullptr;

    std::unique_ptr<Widget> build() {
        auto column = mk<Box>(false);

        auto strip = mk<Strip>("KIEROWNICA G29");
        auto label = mk<Label>("", Font::Small, TEXT_DIM);
        lbl_state = strip->row->add(std::move(label), Pack{.cross = Pack::Center});
        strip->row->add_spacer();
        strip->row->add(dim("Tryb wyjścia"), Pack{.right = 9, .cross = Pack::Center});
        strip->row->add(app_.make_mode_combo(44), Pack{.cross = Pack::Center});
        column->add(std::move(strip), Pack{.bottom = 12});

        auto card = mk<Card>("Przyciski kierownicy G29");
        auto tool_box = mk<Box>(true);
        tool_box->add(mk<Button>("Przypisz po kolei", Button::Accent, [this] { wizard_start(); }));
        tool_box->add(mk<Button>("Przepisz z pada", Button::Small, [this] { copy_from_pad(); }), Pack{.left = 6});
        tool_box->add(mk<Button>("Domyślne", Button::Small, [this] { defaults(); }), Pack{.left = 6});
        tool_box->add(mk<Button>("Wyczyść wszystko", Button::Small, [this] { clear_all(); }), Pack{.left = 6});
        tools = card->right_slot->add(std::move(tool_box));

        auto wiz = mk<Box>(true);
        wiz->add(mk<Button>("Wstecz", Button::Small, [this] { wizard_back(); }));
        wiz->add(mk<Button>("Pomiń", Button::Small, [this] { wizard_advance(+1); }), Pack{.left = 6});
        wiz->add(mk<Button>("Wyczyść i pomiń", Button::Small, [this] { wizard_clear(); }), Pack{.left = 6});
        wiz->add(mk<Button>("Zakończ", Button::Accent, [this] { wizard_stop(); }), Pack{.left = 6});
        wiz->visible = false;
        wizard_tools = card->right_slot->add(std::move(wiz));

        lbl_live = card->body->add(mk<Label>("", Font::Small, GOOD), Pack{.bottom = 6, .cross = Pack::Start});

        PadView::Callbacks cb;
        cb.assign_request = [this](const std::string& c) { on_assign_request(c); };
        cb.clear = [this](const std::string& c) { on_clear(c); };
        view = card->body->add(make_wheel_view(std::move(cb)));
        view->set_assignments(cfg().g29.buttons);
        card->body->add(muted("Te przypisania działają w trybach z G29 (Kierownica G29 oraz G29 + vJoy) i nie zależą od zakładki Pad "
                              "ani od listy vJoy w zakładce Przyciski. Krzyżak bez przypisań bierze kierunki z krzyżaka ustawionego "
                              "w zakładce Przyciski. Biegi to przyciski skrzyni G29 - przypisz je, jeśli masz własną skrzynię albo manetki.", 1080),
                        Pack{.top = 12});
        column->add(std::move(card));
        return column;
    }

    void tick(const std::optional<ProcessedState>& state, bool connected, bool shown) {
        if (state && connected) capture(state->buttons_effective);
        view->tick();
        if (!shown) return;

        auto [text, color] = status(connected);
        lbl_state->set(text, color);
        sync();

        if (!state || !connected) {
            view->set_active({});
            static_cast<WheelCanvas*>(view->canvas())->set_angle(0.0);
            static_cast<WheelCanvas*>(view->canvas())->set_pedals({});
            set_live(std::nullopt);
            return;
        }
        // pokazujemy to, co faktycznie idzie do gry; poza trybem G29 - to, co by poszlo
        auto report = cfg().output.g29_on() ? app_.engine_->last_g29() : std::nullopt;
        protocol::G29Report rep = report ? *report : build_g29_report(*state, cfg());
        auto active = g29_active(rep);
        view->set_active(active);
        static_cast<WheelCanvas*>(view->canvas())->set_angle(state->steer_deg);
        static_cast<WheelCanvas*>(view->canvas())->set_pedals(state->pedals);
        set_live(active);
    }

    void escape() {
        if (!wizard_.empty()) wizard_stop();
        else view->cancel_learning();
    }

    void stop() {
        if (!wizard_.empty()) wizard_stop();
        else if (view->learning()) view->cancel_learning();
    }

    bool wizard_active() const { return !wizard_.empty(); }

    void copy_from_pad() {
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
        sync();
        notify(std::format("Przepisano przypisania z pada - przypisanych kontrolek: {} z {}", filled(), G29_CONTROLS.size()));
        app_.enqueue_log("ok", "G29: przepisano przypisania z zakładki Pad");
    }

private:
    AppConfig& cfg() { return app_.cfg_; }
    static std::string label(const std::string& name) {
        auto it = G29_CONTROL_LABELS.find(name);
        return it == G29_CONTROL_LABELS.end() ? name : it->second;
    }
    int filled() {
        int n = 0;
        for (const auto& [name, value] : cfg().g29.buttons)
            if (value >= 0) ++n;
        return n;
    }
    void sync() { view->set_assignments(cfg().g29.buttons); }

    void notify(const std::string& text, double seconds = 4.0) {
        notice_until_ = monotonic() + seconds;
        lbl_live->set(text, GOOD);
    }

    std::pair<std::string, RGB> status(bool connected) {
        std::string state = app_.g29_state(connected, app_.engine_->status());
        if (state == "offline") return {"Brak połączenia z płytką", MUTED};
        if (state == "old_fw") return {"Firmware bez trybu G29 - wgraj nowy (Diagnostyka → Wgraj firmware)", BAD};
        if (!cfg().output.g29_on()) return {"Tryb G29 wyłączony - gra nie dostaje tych przypisań", WARN};
        if (state == "attaching" || state == "detecting") return {"G29 podłącza się do Windowsa…", WARN};
        if (state == "no_usb") return {"Podłącz kabel do portu Native USB", BAD};
        return {"Windows widzi G29 - przypisania działają w grze", GOOD};
    }

    void set_live(const std::optional<std::map<std::string, bool>>& active) {
        if (monotonic() < notice_until_) return;  // swiezy komunikat ma pierwszenstwo
        const auto& learning = view->learning();
        std::string text;
        RGB color = GOOD;
        if (learning && wizard_.empty()) {
            text = "Czekam na przycisk kierownicy dla: " + label(*learning);
        } else if (learning) {
            text = "";  // w kreatorze krok widac w podpowiedzi nad rysunkiem
        } else if (!active) {
            text = OFFLINE_LIVE;
            color = MUTED;
        } else {
            std::vector<std::string> names;
            for (const auto& n : G29_CONTROLS) {
                auto it = active->find(n);
                if (it != active->end() && it->second) names.push_back(label(n));
            }
            text = "Wciśnięte na G29: " + (names.empty() ? std::string("nic") : join(names, ", "));
        }
        lbl_live->set(text, color);
    }

    void on_assign_request(const std::string&) {
        app_.pages_->select(PanelApp::TAB_G29);
        // druga zakladka nie moze uczyc sie w tym samym czasie
        if (!app_.pad_wizard_.empty()) app_.pad_wizard_stop();
        app_.pad_view_->cancel_learning();
        // przycisk trzymany w chwili klikniecia nie moze od razu wpasc do tej kontrolki
        auto state = app_.engine_->last_state();
        prev_buttons_ = state ? state->buttons_effective : 0;
        notice_until_ = 0;
    }

    void on_clear(const std::string& control) {
        cfg().g29.buttons[control] = -1;
        cfg().normalize();
        sync();
        app_.enqueue_log("info", "G29: wyczyszczono " + label(control));
    }

    void capture(uint32_t mask) {
        auto control = view->learning();
        if (!control) {
            prev_buttons_ = mask;
            return;
        }
        std::vector<int> held;
        for (int i = 0; i < protocol::NUM_BUTTONS; ++i)
            if ((mask >> i) & 1) held.push_back(i);
        view->set_held_buttons(held);

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
                stolen.push_back(label(name));
            }
        buttons[*control] = index;
        cfg().normalize();
        sync();

        std::string note = stolen.empty() ? "" : "  (zabrany z: " + join(stolen, ", ") + ")";
        notify(std::format("Przypisano przycisk {} do: {}{}", index, label(*control), note));
        app_.enqueue_log("ok", std::format("G29: przycisk {} -> {}{}", index, label(*control), note));

        if (!wizard_.empty()) wizard_advance(+1);
        else view->cancel_learning();
    }

    void defaults() {
        stop();
        cfg().g29.buttons = g29_buttons_from_pad(DEFAULT_PAD_BUTTONS);
        cfg().normalize();
        sync();
        app_.enqueue_log("ok", "G29: przywrócono domyślne przypisania");
    }

    void clear_all() {
        stop();
        ButtonMap empty;
        for (const auto& name : G29_CONTROLS) empty[name] = -1;
        cfg().g29.buttons = empty;
        cfg().normalize();
        sync();
        app_.enqueue_log("info", "G29: wyczyszczono wszystkie przypisania");
    }

    void wizard_start() {
        app_.pages_->select(PanelApp::TAB_G29);
        wizard_ = G29_WIZARD_ORDER;
        wizard_i_ = 0;
        tools->set_visible(false);
        wizard_tools->set_visible(true);
        app_.enqueue_log("info", "G29: kreator przypisań - wciskaj kolejne przyciski kierownicy, Esc kończy");
        wizard_show();
    }

    void wizard_show() {
        if (wizard_.empty()) return;
        if (wizard_i_ < 0 || wizard_i_ >= static_cast<int>(wizard_.size())) {
            wizard_stop(true);
            return;
        }
        view->set_wizard(wizard_i_ + 1, static_cast<int>(wizard_.size()));
        view->start_learning(wizard_[static_cast<size_t>(wizard_i_)]);
    }

    void wizard_advance(int delta) {
        if (wizard_.empty()) return;
        wizard_i_ += delta;
        wizard_show();
    }

    void wizard_back() {
        if (!wizard_.empty() && wizard_i_ > 0) wizard_advance(-1);
    }

    void wizard_clear() {
        if (wizard_.empty()) return;
        cfg().g29.buttons[wizard_[static_cast<size_t>(wizard_i_)]] = -1;
        cfg().normalize();
        sync();
        wizard_advance(+1);
    }

    void wizard_stop(bool done = false) {
        if (wizard_.empty()) return;
        wizard_.clear();
        view->set_wizard(std::nullopt, std::nullopt);
        view->cancel_learning();
        wizard_tools->set_visible(false);
        tools->set_visible(true);
        int n = filled();
        notify(std::format("Kreator zakończony - przypisanych kontrolek: {} z {}", n, G29_CONTROLS.size()), 6.0);
        app_.enqueue_log(done ? "ok" : "info", std::format("G29: kreator zakończony ({} z {} kontrolek przypisanych)", n, G29_CONTROLS.size()));
    }

    PanelApp& app_;
    uint32_t prev_buttons_ = 0;
    double notice_until_ = 0;
    Names wizard_;
    int wizard_i_ = 0;
};

// ---------------------------------------------------------------- PanelApp

namespace {
PanelApp* g_app = nullptr;

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->handle_message(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

PanelApp::PanelApp(const AppOptions& options) : options_(options) {
    cfg_ = AppConfig::load();
    std::string load_problem = last_load_problem;
    saved_snapshot_ = config_snapshot();
    engine_ = std::make_unique<WheelEngine>(cfg_, [this](const std::string& l, const std::string& m) { enqueue_log(l, m); });
    title_ = std::string(APP_TITLE) + " " + APP_VERSION;
    g29tab_ = std::make_unique<G29Tab>(*this);

    g_app = this;
    Gfx::get().init();
    enqueue_log("info", "Konfiguracja: " + narrow(config_path().wstring()));
    if (!load_problem.empty()) enqueue_log("warn", load_problem);
}

PanelApp::~PanelApp() { g_app = nullptr; }

std::unique_ptr<Combo> PanelApp::make_mode_combo(int width_chars) {
    std::vector<std::string> labels;
    for (const auto& m : OUTPUT_MODES) labels.push_back(OUTPUT_MODE_LABELS.at(m));
    auto combo = mk<Combo>(labels, index_of(OUTPUT_MODES, cfg_.output.mode), [this](int i) { on_mode_change(i); }, width_chars);
    mode_combos_.push_back(combo.get());
    return combo;
}

void PanelApp::build() {
    header_ = build_header();

    std::vector<std::string> names(TAB_NAMES.begin(), TAB_NAMES.end());
    tabbar_ = mk<TabBar>(names, TAB_ICONS, [this](int i) { go_tab(i); });
    pages_ = mk<Pages>();
    pages_->on_changed = [this](int i) { on_tab_changed(i); };

    auto scrolled = [](std::unique_ptr<Widget> w) { return mk<Scroll>(std::move(w)); };
    pages_->add(scrolled(build_dashboard()));
    pages_->add(scrolled(build_steer_tab()));
    pages_->add(scrolled(build_pedals_tab()));
    pages_->add(scrolled(build_buttons_tab()));
    pages_->add(scrolled(g29tab_->build()));
    pages_->add(scrolled(build_pad_tab()));
    pages_->add(scrolled(build_ffb_tab()));
    pages_->add(scrolled(build_diag_tab()));

    statusbar_ = build_statusbar();
    update_vjoy_text();
    sync_pedal_widgets();
    sync_pad_axis_labels();
    pad_view_->set_assignments(cfg_.pad.buttons);

    if (cfg_.ui.tab > 0 && cfg_.ui.tab < TAB_COUNT) go_tab(cfg_.ui.tab);
}

std::unique_ptr<Widget> PanelApp::build_header() {
    auto row = mk<Box>(true);
    row->add(mk<Logo>(), Pack{.right = 11, .cross = Pack::Center});
    row->add(mk<Label>(APP_TITLE, Font::Title, TEXT), Pack{.cross = Pack::Center});
    row->add(mk<Badge>(std::string("v") + APP_VERSION), Pack{.left = 10, .top = 4, .cross = Pack::Center});
    row->add_spacer();

    row->add(mk<Label>("PORT", Font::TinyBold, MUTED), Pack{.right = 8, .cross = Pack::Center});
    port_combo_ = row->add(mk<Combo>(std::vector<std::string>{}, 0, nullptr, 30), Pack{.right = 7, .cross = Pack::Center});
    row->add(mk<Button>("Odśwież", Button::Ghost, [this] { refresh_ports(); }), Pack{.right = 7, .cross = Pack::Center});
    btn_connect_ = row->add(mk<Button>("Połącz", Button::Accent, [this] { toggle_connect(); }, 86.0f), Pack{.cross = Pack::Center});

    // stan lacza i wyjscia: jeden kafelek, dwa wiersze
    auto tile = mk<Box>(false);
    auto link_row = mk<Box>(true);
    dot_link_ = link_row->add(mk<StatusDot>(10.0f, PANEL_HI), Pack{.right = 7, .cross = Pack::Center});
    auto lbl = mk<Label>("Rozłączono", Font::Small, TEXT_DIM);
    // Stala szerokosc: liczba ramek na sekunde zmienia dlugosc napisu.
    lbl->set_fixed_width(180);
    lbl_link_ = link_row->add(std::move(lbl), Pack{.cross = Pack::Center});
    tile->add(std::move(link_row), Pack{.bottom = 1});
    auto vjoy_row = mk<Box>(true);
    dot_vjoy_ = vjoy_row->add(mk<StatusDot>(10.0f, PANEL_HI), Pack{.right = 7, .cross = Pack::Center});
    auto lbl2 = mk<Label>("vJoy: -", Font::Small, TEXT_DIM);
    lbl2->set_fixed_width(180);
    lbl_vjoy_ = vjoy_row->add(std::move(lbl2), Pack{.cross = Pack::Center});
    tile->add(std::move(vjoy_row), Pack{.top = 1});
    row->add(mk<RoundPanel>(std::move(tile), PANEL_HI, 8.0f, 11.0f, 7.0f, 220.0f), Pack{.left = 16, .cross = Pack::Center});

    refresh_ports();
    return mk<RoundPanel>(std::move(row), PANEL, 12.0f, 16.0f, 12.0f);
}

std::unique_ptr<Widget> PanelApp::build_dashboard() {
    auto grid = mk<Grid>(std::vector<float>{-290.0f, 1.0f});

    // ---------------- lewa kolumna: kierownica ----------------
    auto left = mk<Box>(false);
    auto card = mk<Card>("Kierownica");
    dial_ = card->body->add(mk<WheelDial>(240.0f), Pack{.cross = Pack::Center});
    dial_->set_range(cfg_.steer.range_deg);
    row_counts_ = card->body->add(mk<StatRow>("Impulsy enkodera"), Pack{.top = 10});
    row_axis_ = card->body->add(mk<StatRow>("Oś wyjściowa"));
    row_velocity_ = card->body->add(mk<StatRow>("Prędkość obrotu"));
    auto btns = mk<Box>(true);
    btns->add(mk<Button>("Ustaw środek", Button::Accent, [this] { engine_->center_wheel(); }));
    btns->add(mk<Button>("Reset płytki", Button::Small, [this] { reset_board(); }), Pack{.left = 8});
    card->body->add(std::move(btns), Pack{.top = 10});
    left->add(std::move(card));

    auto quick = mk<Card>("Szybki zakres obrotu");
    auto presets = mk<Grid>(std::vector<float>{1, 1, 1, 1});
    for (size_t i = 0; i < RANGE_PRESETS.size(); ++i) {
        int deg = RANGE_PRESETS[i];
        auto* b = presets->place(mk<Button>(std::format("{}°", deg), Button::Normal, [this, deg] { set_range(deg); }), static_cast<int>(i) / 4, static_cast<int>(i) % 4, 1,
                                 Pack{.left = 3, .top = 3, .right = 3, .bottom = 3});
        range_buttons_[i] = {deg, b};
    }
    quick->body->add(std::move(presets));
    left->add(std::move(quick), Pack{.top = 10});
    grid->place(std::move(left), 0, 0, 1, Pack{.right = 10});

    // ---------------- prawa kolumna ----------------
    auto right = mk<Box>(false);
    auto ped = mk<Card>("Pedały i osie pomocnicze");
    auto gauges = mk<Grid>(std::vector<float>{1, 1, 1, 1, 1, 1});
    for (size_t i = 0; i < NUM_PEDALS; ++i)
        dash_bars_[i] = gauges->place(mk<PedalGauge>(PEDAL_LABELS.at(PEDAL_KEYS[i]), PEDAL_COLORS[i]), 0, static_cast<int>(i), 1, Pack{.cross = Pack::Center});
    ped->body->add(std::move(gauges));
    right->add(std::move(ped));

    auto btn_card = mk<Card>("Przyciski");
    auto row = mk<Box>(true);
    button_grid_ = row->add(mk<ButtonGrid>(), Pack{.flex = 1});
    auto hat_box = mk<Box>(false);
    hat_box->add(muted("Krzyżak"), Pack{.cross = Pack::Center});
    hat_indicator_ = hat_box->add(mk<HatIndicator>(72.0f), Pack{.cross = Pack::Center});
    row->add(std::move(hat_box), Pack{.left = 14});
    btn_card->body->add(std::move(row));
    lbl_dash_out_ = btn_card->body->add(mk<Label>("", Font::Small, TEXT_DIM, 900.0f), Pack{.top = 8});
    right->add(std::move(btn_card), Pack{.top = 10});

    auto out_card = mk<Card>("Wyjście do gier");
    // Tryb zmienia sie przy kazdej zmianie gry, wiec wybor stoi tez na Pulpicie.
    out_card->right_slot->add(make_mode_combo(44));
    lbl_output_ = out_card->body->add(mk<Label>("-", Font::Small, TEXT_DIM, 900.0f));
    // pokazywana dopiero z trescia - pusta zostawiala dziure w karcie
    lbl_output_hint_ = out_card->body->add(mk<Label>("", Font::Small, WARN, 900.0f), Pack{.top = 6});
    lbl_output_hint_->visible = false;
    auto torq = mk<Box>(true);
    torq->add(muted("Moment Force Feedback"), Pack{.right = 10, .cross = Pack::Center});
    torque_meter_ = torq->add(mk<TorqueMeter>(22.0f), Pack{.flex = 1});
    out_card->body->add(std::move(torq), Pack{.top = 8});
    right->add(std::move(out_card), Pack{.top = 10});
    grid->place(std::move(right), 0, 1);
    return grid;
}

std::unique_ptr<Widget> PanelApp::build_steer_tab() {
    auto body = mk<Grid>(std::vector<float>{1, 1});
    const std::vector<std::string> axes(VJOY_AXIS_CHOICES.begin(), VJOY_AXIS_CHOICES.end());

    // --- zakres i enkoder ---
    auto c1 = mk<Card>("Zakres obrotu i enkoder");
    sl_range_ = c1->body->add(mk<Slider>("Zakres lock-to-lock", 90, 1800, cfg_.steer.range_deg, fmt_int("°"), 10,
                                         [this](double v) {
                                             cfg_.steer.range_deg = static_cast<int>(v);
                                             dial_->set_range(v);
                                             engine_->apply_config();
                                         },
                                         "Tyle stopni ma pełny obrót od oporu do oporu. G29 ma fabrycznie 900°."),
                              Pack{.bottom = 10});
    c1->body->add(dim("Impulsy enkodera na obrót kierownicy (x4)"));
    auto row = mk<Box>(true);
    entry_cpr_ = row->add(mk<Entry>(std::to_string(cfg_.steer.counts_per_rev), 12), Pack{.cross = Pack::Center});
    row->add(mk<Button>("Zapisz", Button::Small, [this] { apply_cpr(); }), Pack{.left = 6, .cross = Pack::Center});
    row->add(mk<Button>("Wykryj automatycznie", Button::Small, [this] { detect_cpr(); }), Pack{.left = 6, .cross = Pack::Center});
    c1->body->add(std::move(row), Pack{.top = 3, .bottom = 8});
    c1->body->add(muted("Wykrywanie: ustaw środek, przekręć kierownicę do jednego oporu, potem do drugiego i kliknij przycisk. "
                        "Aplikacja policzy impulsy z przejechanego zakresu.", 900));
    body->place(std::move(c1), 0, 0, 1, Pack{.right = 6, .bottom = 10});

    // --- krzywa i filtr ---
    auto c2 = mk<Card>("Charakterystyka");
    steer_plot_ = c2->body->add(mk<CurvePlot>(104.0f, ACCENT, true, std::array<std::string, 3>{"", "", ""}), Pack{.bottom = 10});
    c2->body->add(mk<Slider>("Liniowość", 0.4, 2.5, cfg_.steer.linearity, fmt_two(), 0.05, [this](double v) { cfg_.steer.linearity = v; },
                             "1.00 = liniowo. Poniżej 1 kierownica reaguje ostrzej przy środku, powyżej 1 łagodniej."),
                  Pack{.bottom = 10});
    c2->body->add(mk<Slider>("Wygładzanie", 0.0, 0.9, cfg_.steer.smoothing, fmt_two(), 0.05, [this](double v) { cfg_.steer.smoothing = v; },
                             "Filtr drgań enkodera. Większa wartość = gładziej, ale z większym opóźnieniem. Zwykle 0."),
                  Pack{.bottom = 10});
    c2->body->add(mk<Checkbox>("Odwróć kierunek obrotu", cfg_.steer.invert,
                               [this](bool v) {
                                   cfg_.steer.invert = v;
                                   engine_->publish_config();
                                   engine_->reset_steer_processor();
                               }),
                  Pack{.cross = Pack::Start});
    auto axis_row = mk<Box>(true);
    axis_row->add(dim("Oś vJoy"), Pack{.cross = Pack::Center});
    axis_row->add(mk<Combo>(axes, index_of(VJOY_AXIS_CHOICES, cfg_.steer.vjoy_axis),
                            [this](int i) {
                                cfg_.steer.vjoy_axis = VJOY_AXIS_CHOICES[static_cast<size_t>(i)];
                                engine_->apply_config();
                            },
                            8),
                  Pack{.left = 8, .right = 8, .cross = Pack::Center});
    axis_row->add(muted("G29 używa osi X"), Pack{.cross = Pack::Center});
    c2->body->add(std::move(axis_row), Pack{.top = 10});
    body->place(std::move(c2), 0, 1, 1, Pack{.left = 6, .bottom = 10});

    // --- miekki ogranicznik ---
    auto c3 = mk<Card>("Miękki ogranicznik końca zakresu",
                       "Działa w firmware, niezależnie od gry - przy przekroczeniu zakresu silnik odpycha kierownicę. Wymaga podłączonego mostka H.", 820.0f);
    c3->body->add(mk<Checkbox>("Włącz miękki ogranicznik", cfg_.steer.soft_endstop,
                               [this](bool v) {
                                   cfg_.steer.soft_endstop = v;
                                   engine_->apply_config();
                               }),
                  Pack{.cross = Pack::Start});
    c3->body->add(mk<Slider>("Siła ogranicznika", 1, 200, cfg_.steer.endstop_gain, fmt_int(), 1,
                             [this](double v) {
                                 cfg_.steer.endstop_gain = static_cast<int>(v);
                                 engine_->apply_config();
                             },
                             "Moment na każdy impuls przekroczenia zakresu."),
                  Pack{.top = 8});
    body->place(std::move(c3), 1, 0, 2);
    return body;
}

std::unique_ptr<Card> PanelApp::build_pedal_card(const std::string& key) {
    int idx = pedal_index(key);
    const size_t i = static_cast<size_t>(idx);
    PedalConfig& p = cfg_.pedals[i];
    PedalWidgets& w = pedal_widgets_[i];
    auto card = mk<Card>(PEDAL_LABELS.at(key));

    auto src = mk<Box>(true);
    src->add(dim("Kanał"), Pack{.cross = Pack::Center});
    std::vector<std::string> values = {"wyłączony"};
    for (int a = 0; a < protocol::NUM_AXES; ++a) values.push_back(std::format("A{}", a));
    w.src = src->add(mk<Combo>(values, p.source + 1,
                               [this, key, idx](int index) {
                                   cfg_.pedals[static_cast<size_t>(idx)].source = index - 1;
                                   engine_->publish_config();
                                   engine_->reset_pedal_processor(key);
                                   engine_->apply_config();
                               },
                               11),
                     Pack{.left = 8, .right = 14, .cross = Pack::Center});
    src->add(dim("Oś vJoy"), Pack{.cross = Pack::Center});
    std::vector<std::string> axes(VJOY_AXIS_CHOICES.begin(), VJOY_AXIS_CHOICES.end());
    w.axis = src->add(mk<Combo>(axes, index_of(VJOY_AXIS_CHOICES, p.vjoy_axis),
                                [this, idx](int index) {
                                    cfg_.pedals[static_cast<size_t>(idx)].vjoy_axis = VJOY_AXIS_CHOICES[static_cast<size_t>(index)];
                                    engine_->apply_config();
                                },
                                7),
                      Pack{.left = 8, .cross = Pack::Center});
    card->body->add(std::move(src));

    w.plot = card->body->add(mk<CurvePlot>(104.0f, PEDAL_COLORS[i], false, std::array<std::string, 3>{"0 %", "wciśnięcie pedału → wyjście do gry", "100 %"}), Pack{.top = 10});
    w.bar = card->body->add(mk<BarMeter>("Wartość wyjściowa", PEDAL_COLORS[i]), Pack{.top = 6, .bottom = 2});
    w.range = card->body->add(mk<StatRow>("Zakres surowy"));
    w.seen = card->body->add(mk<StatRow>("Zaobserwowano"), Pack{.bottom = 6});

    // Zakresy suwakow takie same jak w AppConfig::normalize().
    auto field = [this, idx](double PedalConfig::*member) {
        return [this, idx, member](double v) {
            cfg_.pedals[static_cast<size_t>(idx)].*member = v;
            cfg_.normalize();
        };
    };
    w.dz_lo = card->body->add(mk<Slider>("Martwa strefa - początek", 0.0, 0.45, p.deadzone_low, fmt_pct(), 0.01, field(&PedalConfig::deadzone_low)), Pack{.top = 2, .bottom = 2});
    w.dz_hi = card->body->add(mk<Slider>("Martwa strefa - koniec", 0.0, 0.45, p.deadzone_high, fmt_pct(), 0.01, field(&PedalConfig::deadzone_high)), Pack{.top = 2, .bottom = 2});
    w.gamma = card->body->add(mk<Slider>("Krzywa (gamma)", 0.3, 3.0, p.gamma, fmt_two(), 0.05, field(&PedalConfig::gamma)), Pack{.top = 2, .bottom = 2});
    w.smooth = card->body->add(mk<Slider>("Wygładzanie", 0.0, 0.9, p.smoothing, fmt_two(), 0.05, field(&PedalConfig::smoothing)), Pack{.top = 2, .bottom = 2});
    w.invert = card->body->add(mk<Checkbox>("Odwróć kierunek", p.invert,
                                            [this, idx](bool v) {
                                                cfg_.pedals[static_cast<size_t>(idx)].invert = v;
                                                cfg_.normalize();
                                            }),
                               Pack{.top = 6, .cross = Pack::Start});
    return card;
}

std::unique_ptr<Widget> PanelApp::build_pedals_tab() {
    auto body = mk<Grid>(std::vector<float>{1, 1});
    auto head = mk<Card>("Kreator kalibracji",
                         "1. Kliknij 'Zacznij nasłuchiwanie'.  2. Wciśnij każdy pedał do końca i puść.  3. Kliknij 'Zapisz zakresy'. "
                         "Aplikacja zapamiętuje najmniejszą i największą zaobserwowaną wartość każdego kanału.", 820.0f);
    auto row = mk<Box>(true);
    row->add(mk<Button>("Zacznij nasłuchiwanie", Button::Accent, [this] { calib_start(); }));
    row->add(mk<Button>("Zapisz zakresy", Button::Normal, [this] { calib_apply_all(); }), Pack{.left = 8});
    row->add(mk<Button>("Przywróć pełny zakres", Button::Small, [this] { calib_reset(); }), Pack{.left = 8, .cross = Pack::Center});
    head->body->add(std::move(row));
    lbl_calib_ = head->body->add(mk<Label>("", Font::Small, TEXT_DIM), Pack{.top = 8, .cross = Pack::Start});
    lbl_calib_->visible = false;
    body->place(std::move(head), 0, 0, 2, Pack{.bottom = 10});

    for (int i = 0; i < NUM_PEDALS; ++i)
        body->place(build_pedal_card(PEDAL_KEYS[static_cast<size_t>(i)]), 1 + i / 2, i % 2, 1,
                    i % 2 == 0 ? Pack{.right = 6, .bottom = 10} : Pack{.left = 6, .bottom = 10});
    return body;
}

std::unique_ptr<Widget> PanelApp::build_buttons_tab() {
    auto body = mk<Box>(false);
    auto info = mk<Card>("Mapowanie przycisków",
                         "Lewa kolumna to numer przycisku z płytki (0 = pin 4, 1..31 = piny 22..52). Prawa to numer przycisku w urządzeniu "
                         "vJoy widzianym przez grę. 0 oznacza 'nie wysyłaj'.", 820.0f);
    auto tools = mk<Box>(true);
    tools->add(mk<Button>("Mapowanie 1:1", Button::Small, [this] {
        for (size_t i = 0; i < button_map_.size(); ++i) button_map_[i]->set_index(static_cast<int>(i) + 1);
        on_button_map_change();
        enqueue_log("ok", "Przyciski ustawione 1:1");
    }));
    tools->add(mk<Button>("Wyczyść wszystko", Button::Small, [this] {
        for (Combo* c : button_map_) c->set_index(0);
        on_button_map_change();
        enqueue_log("info", "Mapowanie przycisków wyczyszczone");
    }), Pack{.left = 6});
    tools->add(mk<Button>("Zdejmij wszystkie NC", Button::Small, [this] {
        for (Checkbox* c : button_invert_) c->set(false);
        on_button_invert_change();
        enqueue_log("info", "Zdjęto odwrócenie ze wszystkich przycisków");
    }), Pack{.left = 6});
    info->body->add(std::move(tools));
    lbl_btn_live_ = info->body->add(mk<Label>("", Font::Small, TEXT_DIM), Pack{.top = 8, .cross = Pack::Start});
    lbl_btn_out_ = info->body->add(mk<Label>("", Font::Small, GOOD), Pack{.cross = Pack::Start});
    info->body->add(muted("Kolumna NC odwraca logikę przycisku - dla styków normalnie zwartych (zwarty = puszczony). Diody i wiersz "
                          "'Wciśnięte teraz' pokazują stan JUŻ po odwróceniu, więc efekt widać od razu.", 820.0f), Pack{.top = 6});
    body->add(std::move(info), Pack{.bottom = 10});

    auto hat = mk<Card>("Krzyżak (POV)", "Przycisk przypisany do krzyżaka przestaje być wysyłany jako zwykły przycisk. -1 = nieużywany.", 820.0f);
    auto hrow = mk<Box>(true);
    std::vector<std::string> choices = {"-1"};
    for (int i = 0; i < protocol::NUM_BUTTONS; ++i) choices.push_back(std::to_string(i));
    const char* hat_names[] = {"Góra", "Prawo", "Dół", "Lewo"};
    for (int i = 0; i < 4; ++i) {
        auto cell = mk<Box>(false);
        cell->add(dim(hat_names[i]));
        hat_combos_.push_back(cell->add(mk<Combo>(choices, cfg_.buttons.hat[static_cast<size_t>(i)] + 1, [this](int) { on_hat_change(); }, 6)));
        hrow->add(std::move(cell), Pack{.right = 16});
    }
    hat->body->add(std::move(hrow));
    body->add(std::move(hat), Pack{.bottom = 10});

    auto grid_card = mk<Card>("Przyciski 0 - 31");
    auto g = mk<Grid>(std::vector<float>{0, 0, 0, 0});
    std::vector<std::string> vals;
    for (int i = 0; i <= 128; ++i) vals.push_back(std::to_string(i));
    btn_dots_.resize(protocol::NUM_BUTTONS);
    button_map_.resize(protocol::NUM_BUTTONS);
    button_invert_.resize(protocol::NUM_BUTTONS);
    for (int i = 0; i < protocol::NUM_BUTTONS; ++i) {
        const size_t s = static_cast<size_t>(i);
        auto cell = mk<Box>(true);
        btn_dots_[s] = cell->add(mk<StatusDot>(10.0f, PANEL), Pack{.right = 6, .cross = Pack::Center});
        auto num = mk<Label>(std::format("{:>2}", i), Font::Mono, TEXT_DIM);
        num->set_fixed_width(22);
        cell->add(std::move(num), Pack{.cross = Pack::Center});
        cell->add(muted("→"), Pack{.left = 4, .right = 4, .cross = Pack::Center});
        button_map_[s] = cell->add(mk<Combo>(vals, cfg_.buttons.mapping[s], [this](int) { on_button_map_change(); }, 5), Pack{.cross = Pack::Center});
        button_invert_[s] = cell->add(mk<Checkbox>("NC", cfg_.buttons.invert[s], [this](bool) { on_button_invert_change(); }), Pack{.left = 6, .cross = Pack::Center});
        g->place(std::move(cell), i % 8, i / 8, 1, Pack{.top = 2, .right = 14, .bottom = 2, .cross = Pack::Start});
    }
    grid_card->body->add(std::move(g));
    body->add(std::move(grid_card));
    return body;
}

std::unique_ptr<Widget> PanelApp::build_pad_tab() {
    auto column = mk<Box>(false);

    // Pasek u gory jest celowo plaski - cala wysokosc idzie na liste przypisan.
    auto strip = mk<Strip>("TRYB PADA");
    lbl_pad_state_ = strip->row->add(mk<Label>("", Font::Small, TEXT_DIM), Pack{.cross = Pack::Center});
    strip->row->add_spacer();
    strip->row->add(dim("Tryb wyjścia"), Pack{.left = 24, .right = 9, .cross = Pack::Center});
    strip->row->add(make_mode_combo(44), Pack{.cross = Pack::Center});
    column->add(std::move(strip), Pack{.bottom = 12});

    auto view = mk<Card>("Przypisania");
    auto tools = mk<Box>(true);
    tools->add(mk<Button>("Przypisz po kolei", Button::Accent, [this] { pad_wizard_start(); }));
    tools->add(mk<Button>("Domyślne", Button::Small, [this] { pad_defaults(); }), Pack{.left = 6});
    tools->add(mk<Button>("Wyczyść wszystko", Button::Small, [this] { pad_clear_all(); }), Pack{.left = 6});
    pad_tools_ = view->right_slot->add(std::move(tools));

    auto wiz = mk<Box>(true);
    wiz->add(mk<Button>("Wstecz", Button::Small, [this] { if (!pad_wizard_.empty() && pad_wizard_i_ > 0) pad_wizard_advance(-1); }));
    wiz->add(mk<Button>("Pomiń", Button::Small, [this] { pad_wizard_advance(+1); }), Pack{.left = 6});
    wiz->add(mk<Button>("Wyczyść i pomiń", Button::Small, [this] { pad_wizard_clear(); }), Pack{.left = 6});
    wiz->add(mk<Button>("Zakończ", Button::Accent, [this] { pad_wizard_stop(); }), Pack{.left = 6});
    wiz->visible = false;
    pad_wizard_tools_ = view->right_slot->add(std::move(wiz));

    lbl_pad_live_ = view->body->add(mk<Label>("", Font::Small, GOOD), Pack{.bottom = 6, .cross = Pack::Start});
    lbl_pad_conflict_ = view->body->add(mk<Label>("", Font::Small, WARN, 1080.0f), Pack{.bottom = 6});
    lbl_pad_conflict_->visible = false;

    PadView::Callbacks cb;
    cb.assign_request = [this](const std::string& c) { on_pad_assign_request(c); };
    cb.clear = [this](const std::string& c) { on_pad_clear(c); };
    cb.axis_click = [this](const std::string& d, float x, float y) { on_pad_axis_click(d, x, y); };
    cb.axis_clear = [this](const std::string& d) { set_pad_axis_source(d, ""); };
    cb.cancel = [this] { if (!pad_wizard_.empty()) pad_wizard_stop(); };
    pad_view_ = view->body->add(make_pad_view(std::move(cb)));
    column->add(std::move(view), Pack{.bottom = 12});

    // --- osie ---
    auto axes = mk<Card>("Osie analogowe");
    auto cols = mk<Grid>(std::vector<float>{1, 1});
    auto left = mk<Box>(false);
    auto right = mk<Box>(false);
    sl_pad_range_ = left->add(mk<Slider>("Zakres kierownicy na pełne wychylenie drążka", 45, 1080, cfg_.pad.steer_range_deg, fmt_int("°"), 15,
                                         [this](double v) { cfg_.pad.steer_range_deg = static_cast<int>(v); on_pad_field_changed(); },
                                         "Pad ma mały zakres drążka. Dla gier zręcznościowych 270-450 stopni daje naturalne prowadzenie; pełne 900 sprawi, że auto ledwo skręca."),
                              Pack{.bottom = 10});
    sl_pad_dz_ = left->add(mk<Slider>("Martwa strefa wokół środka", 0.0, 0.40, cfg_.pad.steer_deadzone, fmt_pct(), 0.01,
                                      [this](double v) { cfg_.pad.steer_deadzone = v; on_pad_field_changed(); },
                                      "Własna martwa strefa aplikacji. Zostaw 0, jeśli auto nie drga, gdy trzymasz kierownicę na środku."),
                           Pack{.bottom = 10});
    sl_pad_anti_ = left->add(mk<Slider>("Kompensacja martwej strefy gry", 0.0, 0.60, cfg_.pad.steer_antideadzone, fmt_pct(), 0.01,
                                        [this](double v) { cfg_.pad.steer_antideadzone = v; on_pad_field_changed(); },
                                        "Na tym choruje Forza: gra sama ucina początek wychylenia drążka, więc pierwsze stopnie obrotu kierownicy nic nie robią. "
                                        "Tu najmniejszy ruch od razu przebija próg gry. Najpierw ustaw w grze martwą strefę wewnętrzną skrętu na 0, potem podnoś ten suwak, aż auto reaguje natychmiast."),
                             Pack{.bottom = 10});
    sl_pad_lin_ = left->add(mk<Slider>("Krzywa skrętu", 0.3, 3.0, cfg_.pad.steer_linearity, fmt_two(), 0.05,
                                       [this](double v) { cfg_.pad.steer_linearity = v; on_pad_field_changed(); },
                                       "1.00 = liniowo. Poniżej 1 środek jest czulszy, powyżej 1 spokojniejszy przy środku, a agresywny na końcach."));

    auto prof = mk<Box>(true);
    prof->add(dim("Profil gry"), Pack{.cross = Pack::Center});
    std::vector<std::string> profile_labels;
    for (const auto& k : PAD_PROFILE_ORDER) profile_labels.push_back(PAD_PROFILE_LABELS.at(k));
    pad_profile_ = prof->add(mk<Combo>(profile_labels, index_of(PAD_PROFILE_ORDER, cfg_.pad.detect_profile()), [this](int i) { on_pad_profile(i); }, 28), Pack{.left = 10, .cross = Pack::Center});
    right->add(std::move(prof));
    right->add(muted("Profil ustawia tylko skalowanie skrętu i pedałów pod daną grę. Przypisania przycisków i kierunków zostają nietknięte - zmiana profilu ich nie rusza.", 520.0f), Pack{.top = 8});
    sl_pad_trig_ = right->add(mk<Slider>("Kompensacja martwej strefy spustów", 0.0, 0.60, cfg_.pad.trigger_antideadzone, fmt_pct(), 0.01,
                                         [this](double v) { cfg_.pad.trigger_antideadzone = v; on_pad_field_changed(); },
                                         "To samo dla gazu i hamulca na LT/RT - przydaje się, gdy gra ignoruje pierwsze procenty wciśnięcia pedału."),
                              Pack{.top = 10});
    right->add(mk<Checkbox>("Odwróć kierunek skrętu na padzie", cfg_.pad.steer_invert, [this](bool v) { cfg_.pad.steer_invert = v; }), Pack{.top = 10, .cross = Pack::Start});
    right->add(muted("Spusty Xboxa mają 8 bitów (0-255), więc pedały są tu mniej precyzyjne niż w trybie kierownicy. To ograniczenie XInputa, nie aplikacji.", 520.0f), Pack{.top = 8});
    right->add(muted("Forza, NFS Unbound i inne gry z obsługą kierownic najlepiej działają w trybie Kierownica G29 - widzą wtedy prawdziwą kierownicę i ustawienia pada nie są potrzebne. Tryb pada zostaw dla gier bez kierownic.", 520.0f), Pack{.top = 8});

    cols->place(std::move(left), 0, 0, 1, Pack{.right = 18});
    cols->place(std::move(right), 0, 1);
    axes->body->add(std::move(cols));
    column->add(std::move(axes));
    return column;
}

std::unique_ptr<Widget> PanelApp::build_ffb_tab() {
    auto body = mk<Grid>(std::vector<float>{1, 1});
    FfbConfig& f = cfg_.ffb;

    auto field = [this](double FfbConfig::*member) {
        return [this, member](double v) {
            cfg_.ffb.*member = v;
            cfg_.normalize();
        };
    };
    auto int_field = [this](int FfbConfig::*member) {
        return [this, member](double v) {
            cfg_.ffb.*member = static_cast<int>(v);
            cfg_.normalize();
            // PWM maksymalne musi byc wieksze od minimalnego - normalize()
            // podnosi je wtedy, a suwak ma pokazac to samo
            if (member == &FfbConfig::min_duty || member == &FfbConfig::max_duty) sl_maxduty_->set(cfg_.ffb.max_duty);
        };
    };

    auto warn = mk<Card>("Zanim włączysz",
                         "Force Feedback steruje silnikiem przez mostek H na pinach 5 (EN), 6 (RPWM) i 7 (LPWM). Jeśli nie masz podłączonego silnika, "
                         "możesz włączyć FFB bezpiecznie - nic się nie stanie. Przy pierwszym uruchomieniu z silnikiem zacznij od małego wzmocnienia i trzymaj ręce na kierownicy.", 820.0f);
    chk_ffb_on_ = warn->body->add(mk<Checkbox>("Włącz Force Feedback", f.enabled,
                                               [this](bool enabled) {
                                                   engine_->set_ffb_enabled(enabled);
                                                   enqueue_log(enabled ? "ok" : "info", enabled ? "Force Feedback włączony" : "Force Feedback wyłączony");
                                               }),
                                  Pack{.cross = Pack::Start});
    lbl_ffb_state_ = warn->body->add(mk<Label>("", Font::Small, TEXT_DIM, 820.0f), Pack{.top = 6});
    warn->body->add(muted("Moment wysyłany do płytki"), Pack{.top = 10, .cross = Pack::Start});
    ffb_meter_ = warn->body->add(mk<TorqueMeter>(26.0f), Pack{.top = 3});
    body->place(std::move(warn), 0, 0, 2, Pack{.bottom = 10});

    // --- sila ---
    auto c1 = mk<Card>("Siła");
    c1->body->add(mk<Slider>("Wzmocnienie globalne", 0.0, 1.0, f.overall_gain, fmt_pct(), 0.01, field(&FfbConfig::overall_gain)), Pack{.bottom = 8});
    c1->body->add(mk<Slider>("Ograniczenie maksimum", 0.05, 1.0, f.max_force, fmt_pct(), 0.01, field(&FfbConfig::max_force), "Chroni silnik i zasilacz - obcina szczytowy moment."), Pack{.bottom = 8});
    c1->body->add(mk<Slider>("Siła minimalna", 0.0, 0.5, f.min_force, fmt_pct(), 0.01, field(&FfbConfig::min_force), "Dodawana do każdego niezerowego momentu, żeby pokonać tarcie przekładni. Za duża wartość daje szarpanie."), Pack{.bottom = 8});
    c1->body->add(mk<Slider>("Martwa strefa momentu", 0.0, 0.2, f.deadband, fmt_pct(1), 0.005, field(&FfbConfig::deadband)), Pack{.bottom = 8});
    c1->body->add(mk<Checkbox>("Odwróć kierunek działania silnika", f.invert, [this](bool v) { cfg_.ffb.invert = v; }), Pack{.cross = Pack::Start});
    body->place(std::move(c1), 1, 0, 1, Pack{.right = 6, .bottom = 10});

    // --- efekty wlasne ---
    auto c2 = mk<Card>("Efekty generowane lokalnie", "Działają także w grach, które nie wysyłają FFB.");
    c2->body->add(mk<Slider>("Sprężyna (samocentrowanie)", 0.0, 1.0, f.spring_gain, fmt_pct(), 0.01, field(&FfbConfig::spring_gain)), Pack{.bottom = 8});
    c2->body->add(mk<Slider>("Tłumienie", 0.0, 1.0, f.damper_gain, fmt_pct(), 0.01, field(&FfbConfig::damper_gain)), Pack{.bottom = 8});
    c2->body->add(mk<Slider>("Tarcie", 0.0, 1.0, f.friction_gain, fmt_pct(), 0.01, field(&FfbConfig::friction_gain)), Pack{.bottom = 8});
    body->place(std::move(c2), 1, 1, 1, Pack{.left = 6, .bottom = 10});

    // --- efekty z gry ---
    auto c3 = mk<Card>("Skalowanie efektów z gry");
    c3->body->add(mk<Slider>("Siła stała / narastanie", 0.0, 2.0, f.game_constant_gain, fmt_pct(), 0.05, field(&FfbConfig::game_constant_gain)), Pack{.bottom = 8});
    c3->body->add(mk<Slider>("Efekty okresowe (wibracje)", 0.0, 2.0, f.game_periodic_gain, fmt_pct(), 0.05, field(&FfbConfig::game_periodic_gain)), Pack{.bottom = 8});
    c3->body->add(mk<Slider>("Sprężyna z gry", 0.0, 2.0, f.game_spring_gain, fmt_pct(), 0.05, field(&FfbConfig::game_spring_gain)), Pack{.bottom = 8});
    c3->body->add(mk<Slider>("Tłumienie i tarcie z gry", 0.0, 2.0, f.game_damper_gain, fmt_pct(), 0.05, field(&FfbConfig::game_damper_gain)), Pack{.bottom = 8});
    c3->body->add(dim("Interpretacja kierunku"), Pack{.top = 4});
    std::vector<std::string> dir_labels;
    for (const auto& m : DIRECTION_MODES) dir_labels.push_back(DIRECTION_LABELS.at(m));
    c3->body->add(mk<Combo>(dir_labels, index_of(DIRECTION_MODES, f.direction_mode),
                            [this, dir_labels](int i) {
                                // w konfiguracji - wczesniej wybor przepadal przy restarcie
                                cfg_.ffb.direction_mode = DIRECTION_MODES[static_cast<size_t>(i)];
                                engine_->publish_config();
                                enqueue_log("info", "Tryb kierunku FFB: " + dir_labels[static_cast<size_t>(i)]);
                            },
                            36),
                  Pack{.top = 3, .cross = Pack::Start});
    c3->body->add(muted("Gry różnie kodują kierunek siły. Jeśli FFB działa odwrotnie albo tylko w jedną stronę, zmień ten tryb.", 900.0f), Pack{.top = 4});
    body->place(std::move(c3), 2, 0, 1, Pack{.right = 6, .bottom = 10});

    // --- mostek H ---
    auto c4 = mk<Card>("Mostek H i bezpieczeństwo");
    c4->body->add(mk<Slider>("PWM minimalne", 0, 254, f.min_duty, fmt_int(), 1, int_field(&FfbConfig::min_duty)), Pack{.bottom = 8});
    sl_maxduty_ = c4->body->add(mk<Slider>("PWM maksymalne", 1, 255, f.max_duty, fmt_int(), 1, int_field(&FfbConfig::max_duty)), Pack{.bottom = 8});
    c4->body->add(mk<Slider>("Max zmiana momentu na ms", 256, 65280, f.slew_per_ms, fmt_int(), 256, int_field(&FfbConfig::slew_per_ms), "Ogranicza szarpnięcia. 65280 praktycznie wyłącza limit."), Pack{.bottom = 8});
    c4->body->add(mk<Slider>("Watchdog płytki", 20, 1000, f.timeout_ms, fmt_int(" ms"), 10, int_field(&FfbConfig::timeout_ms), "Po tym czasie bez komendy z PC firmware zwalnia silnik."), Pack{.bottom = 8});
    c4->body->add(mk<Slider>("Częstotliwość FFB", 50, 1000, f.update_hz, fmt_int(" Hz"), 10, int_field(&FfbConfig::update_hz)));
    body->place(std::move(c4), 2, 1, 1, Pack{.left = 6, .bottom = 10});
    return body;
}

std::unique_ptr<Card> PanelApp::build_g29_card() {
    auto card = mk<Card>("Kierownica G29 (Native USB)");
    const std::pair<const char*, const char*> rows[] = {{"fw", "Firmware z trybem G29"}, {"usb", "Windows widzi G29"}, {"feed", "Raporty z aplikacji"}, {"game", "Komendy od gry"}};
    for (const auto& [key, label] : rows) g29_rows_[key] = card->body->add(mk<StatRow>(label));
    lbl_g29_report_ = card->body->add(mk<Label>("", Font::Mono, TEXT_DIM), Pack{.top = 8, .cross = Pack::Start});
    card->body->add(mk<Checkbox>("Pedały jak w oryginalnym G29 (255 = puszczony)", cfg_.g29.pedals_inverted, [this](bool v) { cfg_.g29.pedals_inverted = v; }), Pack{.top = 8, .cross = Pack::Start});
    card->body->add(muted("Podłącz dwa kable: Programming Port (łącze z tą aplikacją) i Native USB (kierownica dla gier). Gdy aplikacja jest zamknięta, "
                          "G29 stoi w pozycji spoczynkowej - kalibrację i mapowanie liczy tylko ona.", 700.0f), Pack{.top = 8});
    card->body->add(muted(G29_BUTTON_HELP, 700.0f), Pack{.top = 6});
    return card;
}

std::unique_ptr<Widget> PanelApp::build_diag_tab() {
    auto body = mk<Grid>(std::vector<float>{-290.0f, 1.0f});

    // --- lewa kolumna: lacze nad uruchamianiem ---
    auto leftcol = mk<Box>(false);
    auto c1 = mk<Card>("Łącze szeregowe");
    const std::pair<const char*, const char*> rows[] = {{"port", "Port"}, {"fw", "Firmware"}, {"fps", "Ramki na sekundę"}, {"bps", "Przepływ"}, {"lat", "Opóźnienie"},
                                                        {"crc", "Błędy CRC"}, {"drop", "Zgubione ramki"}, {"resync", "Resynchronizacje"}, {"total", "Ramek łącznie"}, {"up", "Czas pracy"}};
    for (const auto& [key, label] : rows) diag_rows_[key] = c1->body->add(mk<StatRow>(label));
    c1->body->add(muted("Ramki / s"), Pack{.top = 10, .bottom = 2, .cross = Pack::Start});
    spark_fps_ = c1->body->add(mk<Sparkline>(40.0f, GOOD));
    c1->body->add(muted("Opóźnienie [ms]"), Pack{.top = 8, .bottom = 2, .cross = Pack::Start});
    spark_lat_ = c1->body->add(mk<Sparkline>(40.0f, WARN));
    c1->body->add(mk<Slider>("Częstotliwość ramek płytki", 50, 500, cfg_.link.report_hz, fmt_int(" Hz"), 10,
                             [this](double v) {
                                 cfg_.link.report_hz = static_cast<int>(v);
                                 engine_->apply_config();
                             }),
                  Pack{.top = 10});
    // wgrywanie firmware bez zamykania panelu - port zwalnia sam panel
    btn_flash_ = c1->body->add(mk<Button>(std::string("Wgraj firmware ") + protocol::FW_LATEST_TEXT, Button::Small, [this] { flash_firmware(); }), Pack{.top = 14, .cross = Pack::Start});
    {
        std::error_code ec;
        if (!fs::is_regular_file(firmware_script(), ec)) btn_flash_->set_enabled(false);
    }
    c1->body->add(muted("Zwalnia port, otwiera okno wgrywania (wgraj_firmware.bat) i łączy się ponownie, gdy je zamkniesz.", 260.0f), Pack{.top = 4});
    leftcol->add(std::move(c1));

    auto c0 = mk<Card>("Uruchamianie");
    chk_autostart_ = c0->body->add(mk<Checkbox>("Uruchamiaj razem z Windowsem", false, [this](bool v) { on_autostart(v); }), Pack{.cross = Pack::Start});
    c0->body->add(muted("Panel startuje zminimalizowany i sam łączy się z płytką. W trybie G29 gry widzą kierownicę tylko wtedy, gdy panel działa.", 260.0f), Pack{.top = 4});
    lbl_autostart_other_ = c0->body->add(mk<Label>("", Font::Small, WARN, 260.0f), Pack{.top = 6});
    lbl_autostart_other_->visible = false;
    leftcol->add(std::move(c0), Pack{.top = 10});
    sync_autostart();
    body->place(std::move(leftcol), 0, 0, 1, Pack{.right = 10});

    // --- prawa kolumna: G29 nad vJoy ---
    auto rightcol = mk<Box>(false);
    rightcol->add(build_g29_card(), Pack{.bottom = 10});

    auto c2 = mk<Card>("Urządzenie wirtualne (vJoy)");
    auto row = mk<Box>(true);
    row->add(dim("Numer urządzenia"), Pack{.cross = Pack::Center});
    std::vector<std::string> ids;
    for (int i = 1; i <= 16; ++i) ids.push_back(std::to_string(i));
    row->add(mk<Combo>(ids, cfg_.output.vjoy_device - 1,
                       [this](int i) {
                           cfg_.output.vjoy_device = i + 1;
                           reopen_output();
                       },
                       5),
             Pack{.left = 8, .right = 8, .cross = Pack::Center});
    row->add(mk<Button>("Podłącz ponownie", Button::Small, [this] { reopen_output(true); }), Pack{.right = 6, .cross = Pack::Center});
    row->add(mk<Button>("Sprawdź vJoy", Button::Small, [this] { update_vjoy_text(); go_tab(TAB_DIAG); }), Pack{.cross = Pack::Center});
    c2->body->add(std::move(row));
    c2->body->add(dim("Tryb wyjścia"), Pack{.top = 8, .cross = Pack::Start});
    c2->body->add(make_mode_combo(46), Pack{.top = 3, .cross = Pack::Start});
    c2->body->add(mk<Checkbox>("Ukrywaj przed grami urządzenie, którego tryb nie używa (G29 albo vJoy)", cfg_.output.hide_unused,
                               [this](bool v) {
                                   cfg_.output.hide_unused = v;
                                   reopen_output();
                               }),
                  Pack{.top = 6, .cross = Pack::Start});
    txt_vjoy_ = c2->body->add(mk<TextView>(9, false), Pack{.top = 10});
    auto hint = mk<Box>(true);
    hint->add(mk<Button>("Pobierz vJoy", Button::Small, [] { ShellExecuteW(nullptr, L"open", widen(VJOY_URL).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }), Pack{.cross = Pack::Center});
    hint->add(muted("Potrzebne tylko w trybach z vJoy - tryb G29 działa bez niego"), Pack{.left = 8, .cross = Pack::Center});
    c2->body->add(std::move(hint), Pack{.top = 8});
    rightcol->add(std::move(c2));
    body->place(std::move(rightcol), 0, 1);

    // --- co ktora gra potrafi zobaczyc ---
    auto c25 = mk<Card>("Zgodność z grami");
    auto table = mk<Grid>(std::vector<float>{0.0f, 0.0f, 0.0f, 1.0f}, std::vector<float>{0.0f, 300.0f});
    int row_i = 0;
    for (const GameRow& g : GAME_COMPAT) {
        table->place(mk<Tick>(row_i == 0 ? ACCENT : BORDER_HI, 3.0f, 14.0f), row_i, 0, 1, Pack{.top = 3, .right = 9, .bottom = 3});
        table->place(dim(g.games), row_i, 1, 1, Pack{.top = 3, .bottom = 3, .cross = Pack::Start});
        table->place(mk<Label>(g.mode, Font::Bold, ACCENT), row_i, 2, 1, Pack{.left = 14, .top = 3, .right = 14, .bottom = 3, .cross = Pack::Start});
        table->place(muted(g.why, 560.0f), row_i, 3, 1, Pack{.top = 3, .bottom = 3});
        ++row_i;
    }
    c25->body->add(std::move(table));
    c25->body->add(muted("Force Feedback z gry działa tylko w trybie vJoy - tamta droga idzie HID PID przez sterownik vJoy. W trybie G29 gra wysyła FFB do sterownika "
                         "Logitecha, którego tu nie ma, a w trybie pada XInput w ogóle nie ma kanału zwrotnego. W obu zostają efekty wyliczane lokalnie (sprężyna, tłumienie, tarcie) "
                         "z zakładki Force Feedback.", 1040.0f), Pack{.top = 10});
    body->place(std::move(c25), 1, 0, 2, Pack{.top = 10});

    // --- dziennik ---
    auto c3 = mk<Card>("Dziennik");
    txt_log_ = c3->body->add(mk<TextView>(9, true));
    auto tools = mk<Box>(true);
    tools->add(mk<Button>("Wyczyść", Button::Small, [this] { clear_log(); }), Pack{.cross = Pack::Center});
    tools->add(mk<Button>("Kopiuj do schowka", Button::Small, [this] { copy_log(); }), Pack{.left = 6, .cross = Pack::Center});
    lbl_log_copied_ = tools->add(mk<Label>("", Font::Small, GOOD), Pack{.left = 10, .cross = Pack::Center});
    c3->body->add(std::move(tools), Pack{.top = 8});
    body->place(std::move(c3), 2, 0, 2, Pack{.top = 10});
    return body;
}

std::unique_ptr<Widget> PanelApp::build_statusbar() {
    auto bar = mk<Box>(true);
    auto status = mk<Label>("", Font::Small, MUTED);
    lbl_status_ = bar->add(std::move(status), Pack{.flex = 1, .cross = Pack::Center});
    lbl_saved_ = bar->add(mk<Label>(SAVE_HINT, Font::Small, MUTED), Pack{.right = 12, .cross = Pack::Center});
    bar->add(mk<Button>("Zapisz ustawienia", Button::Accent, [this] { save_config(); }), Pack{.cross = Pack::Center});
    return bar;
}

// --------------------------------------------------------------- polecenia

void PanelApp::go_tab(int index) { pages_->select(index); }

void PanelApp::on_tab_changed(int index) {
    tabbar_->select(index);
    if (index != TAB_G29) g29tab_->stop();
    // Nauka trwala dalej na innej zakladce: przycisk wcisniety np. na Pulpicie
    // po cichu trafial do pada.
    if (pad_view_ && index != TAB_PAD) pad_stop_learning();
}

void PanelApp::refresh_ports() {
    std::vector<std::string> labels = {"Automatycznie (wykryj płytkę)"};
    port_devices_ = {""};
    int chosen = 0;
    for (const PortInfo& p : list_ports()) {
        labels.push_back(p.label());
        port_devices_.push_back(p.device);
        if (!p.device.empty() && p.device == cfg_.link.port) chosen = static_cast<int>(labels.size()) - 1;
    }
    if (port_combo_) port_combo_->set_items(labels, chosen);
}

std::string PanelApp::selected_port() const {
    int i = port_combo_->index();
    return (i >= 0 && i < static_cast<int>(port_devices_.size())) ? port_devices_[static_cast<size_t>(i)] : std::string();
}

void PanelApp::toggle_connect() {
    if (flash_proc_) {
        // skrypt wgrywania potrzebuje portu - panel nie moze go zajac
        enqueue_log("warn", "Trwa wgrywanie firmware - panel połączy się sam po zamknięciu okna wgrywania");
        return;
    }
    LinkState state = engine_->link().state();
    if (state == LinkState::Connected || state == LinkState::Scanning) {
        engine_->disconnect();
        enqueue_log("info", "Rozłączono ręcznie");
    } else {
        cfg_.link.port = selected_port();
        engine_->connect(cfg_.link.port.empty() ? std::nullopt : std::optional<std::string>(cfg_.link.port));
    }
}

bool PanelApp::ask(const char* title, const std::string& text) {
    if (!hwnd_) return true;
    return MessageBoxW(hwnd_, widen(text).c_str(), widen(title).c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
}

void PanelApp::inform(const char* title, const std::string& text, UINT icon) {
    if (hwnd_) MessageBoxW(hwnd_, widen(text).c_str(), widen(title).c_str(), MB_OK | icon);
}

void PanelApp::flash_firmware() {
    if (flash_proc_) return;
    fs::path script = firmware_script();
    std::error_code ec;
    if (!fs::is_regular_file(script, ec)) {
        inform("Wgraj firmware", "Nie znaleziono skryptu:\n" + narrow(script.wstring()), MB_ICONWARNING);
        return;
    }
    if (!ask("Wgraj firmware", std::format("Wgrać firmware {} z katalogu firmware/kierownica?\n\nPanel rozłączy się z płytką na czas wgrywania "
                                           "(gra straci kierownicę) i połączy się ponownie, gdy zamkniesz okno wgrywania.", protocol::FW_LATEST_TEXT)))
        return;
    // Port z dzialajacego polaczenia - skrypt sam szuka tylko oryginalnego Due
    // (VID 2341/2A03), a klon z innym ukladem USB by przeoczyl.
    LinkState state = engine_->link().state();
    std::string port = state == LinkState::Connected ? engine_->link_stats().port : cfg_.link.port;
    flash_reconnect_ = state == LinkState::Connected || state == LinkState::Scanning;
    engine_->disconnect();

    std::wstring cmd = L"cmd.exe /c \"\"" + script.wstring() + L"\"";
    if (!port.empty()) cmd += L" " + widen(port);
    cmd += L"\"";
    std::wstring cwd = script.parent_path().wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, cwd.c_str(), &si, &pi)) {
        inform("Wgraj firmware", win_error_text(GetLastError()), MB_ICONERROR);
        if (flash_reconnect_) engine_->connect();
        return;
    }
    CloseHandle(pi.hThread);
    flash_proc_ = pi.hProcess;
    btn_flash_->set_enabled(false);
    std::string where = port.empty() ? "(skrypt sam znajdzie port)" : "przez " + port;
    enqueue_log("info", "Wgrywanie firmware " + where + " - panel połączy się po zamknięciu okna wgrywania");
}

void PanelApp::poll_flash() {
    if (!flash_proc_ || WaitForSingleObject(flash_proc_, 0) != WAIT_OBJECT_0) return;
    CloseHandle(flash_proc_);
    flash_proc_ = nullptr;
    btn_flash_->set_enabled(true);
    enqueue_log("info", "Okno wgrywania zamknięte");
    if (flash_reconnect_) engine_->connect();
}

// Zaznaczone tylko wtedy, gdy autostart wskazuje te kopie panelu. Inna kopia
// (np. wersja Python) dostaje ostrzezenie zamiast po cichu przejac wpis.
void PanelApp::sync_autostart() {
    auto command = winsys::autostart_get();
    std::optional<std::wstring> target = command ? winsys::autostart_target(*command) : std::nullopt;
    bool ours = target && lower(*target) == lower(exe_path().wstring());
    chk_autostart_->set(ours);
    if (command && !ours) {
        lbl_autostart_other_->set("Teraz z Windowsem startuje inna kopia panelu: " + narrow(target ? *target : *command) + ". Zaznaczenie przełączy autostart na tę kopię.");
        lbl_autostart_other_->set_visible(true);
    } else {
        lbl_autostart_other_->set_visible(false);
    }
}

void PanelApp::on_autostart(bool want) {
    if (!winsys::autostart_set(want ? std::optional<std::wstring>(winsys::autostart_command(exe_path())) : std::nullopt)) {
        inform("Uruchamianie z Windowsem", "Nie udało się zmienić autostartu.", MB_ICONERROR);
    } else {
        enqueue_log(want ? "ok" : "info", want ? "Panel będzie startował razem z Windowsem" : "Panel nie startuje już razem z Windowsem");
    }
    sync_autostart();
}

void PanelApp::reset_board() {
    if (!engine_->is_connected()) {
        // bez lacza reset nigdzie nie pojdzie - nie wylaczamy tez FFB
        inform("Reset płytki", "Brak połączenia z płytką.", MB_ICONINFORMATION);
        return;
    }
    if (ask("Reset płytki", "Wysłać reset do Arduino?\n\nForce Feedback zostanie wyłączony, a środek kierownicy ustawiony na bieżącą pozycję.")) {
        engine_->reset_board();
        chk_ffb_on_->set(false);
        cfg_.ffb.enabled = false;
    }
}

void PanelApp::set_range(int degrees) {
    cfg_.steer.range_deg = degrees;
    sl_range_->set(degrees);
    dial_->set_range(degrees);
    engine_->apply_config();
    enqueue_log("ok", std::format("Zakres obrotu: {} stopni", degrees));
}

void PanelApp::apply_cpr() {
    json::Value text(entry_cpr_->text());
    int64_t value = text.to_int(INT64_MIN);
    if (value == INT64_MIN) {
        inform("Impulsy enkodera", "Podaj liczbę całkowitą.", MB_ICONWARNING);
        return;
    }
    if (value < 4) {
        inform("Impulsy enkodera", "Wartość musi być co najmniej 4.", MB_ICONWARNING);
        return;
    }
    cfg_.steer.counts_per_rev = static_cast<int>(std::min<int64_t>(value, 2'000'000));
    engine_->apply_config();
    entry_cpr_->set(std::to_string(cfg_.steer.counts_per_rev));
    enqueue_log("ok", std::format("Impulsy na obrót: {}", cfg_.steer.counts_per_rev));
}

void PanelApp::detect_cpr() {
    auto cpr = engine_->detect_counts_per_rev();
    if (!cpr) {
        inform("Wykrywanie enkodera", "Za mały przejechany zakres.\n\nUstaw środek, przekręć kierownicę do jednego oporu, potem do drugiego i kliknij ponownie.", MB_ICONINFORMATION);
        return;
    }
    cfg_.steer.counts_per_rev = *cpr;
    entry_cpr_->set(std::to_string(*cpr));
    engine_->apply_config();
    enqueue_log("ok", std::format("Wykryto {} impulsów na obrót kierownicy", *cpr));
}

void PanelApp::calib_start() {
    engine_->reset_raw_tracking();
    set_calib_text("Nasłuchiwanie... wciśnij każdy pedał do końca i puść, potem kliknij 'Zapisz zakresy'.");
    enqueue_log("info", "Rozpoczęto nasłuchiwanie kalibracji pedałów");
}

void PanelApp::calib_apply_all() {
    std::vector<std::string> ok, skipped;
    for (const auto& key : PEDAL_KEYS) {
        if (cfg_.pedal(key).source < 0) continue;
        (engine_->calibrate_pedal_from_seen(key) ? ok : skipped).push_back(PEDAL_LABELS.at(key));
    }
    sync_pedal_widgets();
    if (!ok.empty()) enqueue_log("ok", "Zapisano kalibrację: " + join(ok, ", "));
    if (!skipped.empty()) enqueue_log("warn", "Za mały zakres, pominięto: " + join(skipped, ", "));
    set_calib_text("Zapisano: " + (ok.empty() ? std::string("nic") : join(ok, ", ")) + (skipped.empty() ? "" : " | pominięto: " + join(skipped, ", ")));
}

void PanelApp::set_calib_text(const std::string& text) {
    lbl_calib_->set(text);
    lbl_calib_->set_visible(true);
}

void PanelApp::calib_reset() {
    for (PedalConfig& p : cfg_.pedals) {
        p.raw_min = 0;
        p.raw_max = 4095;
    }
    engine_->reset_raw_tracking();
    sync_pedal_widgets();
    enqueue_log("info", "Przywrócono pełny zakres surowy (0-4095)");
}

void PanelApp::sync_pedal_widgets() {
    for (size_t i = 0; i < NUM_PEDALS; ++i) {
        const PedalConfig& c = cfg_.pedals[i];
        PedalWidgets& w = pedal_widgets_[i];
        w.range->set(std::format("{} - {}", c.raw_min, c.raw_max));
        w.dz_lo->set(c.deadzone_low);
        w.dz_hi->set(c.deadzone_high);
        w.gamma->set(c.gamma);
        w.smooth->set(c.smoothing);
        w.invert->set(c.invert);
    }
}

void PanelApp::on_button_map_change() {
    for (size_t i = 0; i < button_map_.size(); ++i) cfg_.buttons.mapping[i] = button_map_[i]->index();
    cfg_.normalize();
}

void PanelApp::on_button_invert_change() {
    for (size_t i = 0; i < button_invert_.size(); ++i) cfg_.buttons.invert[i] = button_invert_[i]->value();
    cfg_.normalize();
}

void PanelApp::on_hat_change() {
    for (size_t i = 0; i < hat_combos_.size(); ++i) cfg_.buttons.hat[i] = hat_combos_[i]->index() - 1;
    cfg_.normalize();
}

void PanelApp::on_mode_change(int index) {
    cfg_.output.mode = OUTPUT_MODES[static_cast<size_t>(index)];
    sync_mode_widgets();
    reopen_output();
    enqueue_log("info", "Tryb wyjścia: " + OUTPUT_MODE_LABELS.at(cfg_.output.mode));
}

void PanelApp::sync_mode_widgets() {
    int index = index_of(OUTPUT_MODES, cfg_.output.mode);
    for (Combo* c : mode_combos_) c->set_index(index);
}

void PanelApp::on_pad_field_changed() {
    cfg_.normalize();
    sync_pad_profile_label();
}

// Reczny ruch suwaka przestawia nazwe profilu na wlasny.
void PanelApp::sync_pad_profile_label() {
    if (pad_profile_) pad_profile_->set_index(index_of(PAD_PROFILE_ORDER, cfg_.pad.detect_profile()));
}

void PanelApp::on_pad_profile(int index) {
    const std::string& key = PAD_PROFILE_ORDER[static_cast<size_t>(index)];
    if (key.empty()) {
        // Wlasny nie jest zestawem wartosci - nazwa musi wrocic do tego, co
        // naprawde jest na suwakach.
        sync_pad_profile_label();
        return;
    }
    cfg_.pad.apply_profile(key);
    cfg_.normalize();
    // set() nie wywoluje callbacku, wiec nie przestawi profilu na wlasny
    sl_pad_range_->set(cfg_.pad.steer_range_deg);
    sl_pad_dz_->set(cfg_.pad.steer_deadzone);
    sl_pad_anti_->set(cfg_.pad.steer_antideadzone);
    sl_pad_lin_->set(cfg_.pad.steer_linearity);
    sl_pad_trig_->set(cfg_.pad.trigger_antideadzone);
    enqueue_log("info", "Profil pada: " + PAD_PROFILE_LABELS.at(key));
}

bool PanelApp::pad_source_available(const std::string& source) const {
    int idx = pedal_index(source);
    return idx < 0 || cfg_.pedals[static_cast<size_t>(idx)].source >= 0;
}

std::string PanelApp::pad_axis_label(const std::string& direction) const {
    std::string source = cfg_.pad.source_of(direction);
    if (source.empty()) return "";
    const std::string& label = PAD_SOURCE_LABELS.at(source);
    return pad_source_available(source) ? label : label + " (!)";
}

void PanelApp::set_pad_axis_source(const std::string& direction, const std::string& source) {
    cfg_.pad.axes[direction] = source;
    cfg_.normalize();
    sync_pad_axis_labels();
    const std::string& name = PAD_DIRECTION_LABELS.at(direction);
    if (!source.empty()) enqueue_log("ok", "Pad: " + PAD_SOURCE_LABELS.at(source) + " -> " + name);
    else enqueue_log("info", "Pad: wyczyszczono " + name);
}

void PanelApp::sync_pad_axis_labels() {
    LabelMap labels;
    for (const auto& d : PAD_DIRECTIONS) labels[d] = pad_axis_label(d);
    pad_view_->set_axis_labels(labels);
}

void PanelApp::on_pad_axis_click(const std::string& direction, float x, float y) {
    std::string current = cfg_.pad.source_of(direction);
    PopupMenu menu(upper_pl(PAD_DIRECTION_LABELS.at(direction)));
    for (const auto& key : PAD_SOURCE_KEYS)
        menu.add(PAD_SOURCE_LABELS.at(key), [this, direction, key] { set_pad_axis_source(direction, key); }, key == current,
                 pad_source_available(key) ? "" : "kanał wyłączony");
    menu.add_separator();
    menu.add("Nie używaj", [this, direction] { set_pad_axis_source(direction, ""); }, current.empty());
    menu.popup(x, y);
}

void PanelApp::pad_notify(const std::string& text, double seconds) {
    pad_notice_until_ = monotonic() + seconds;
    lbl_pad_live_->set(text, GOOD);
}

void PanelApp::on_escape() {
    if (Ui::get().overlay()) {
        Ui::get().close_overlay();
        return;
    }
    if (pages_->current() == TAB_G29) g29tab_->escape();
    else pad_stop_learning();
}

void PanelApp::pad_stop_learning() {
    if (!pad_wizard_.empty()) pad_wizard_stop();
    else pad_view_->cancel_learning();
}

void PanelApp::on_pad_assign_request(const std::string& control) {
    pages_->select(TAB_PAD);
    g29tab_->stop();
    if (!pad_wizard_.empty()) {
        // Klik w inna kontrolke w trakcie kreatora: kreator idzie za nim.
        if (contains(pad_wizard_, control) && pad_wizard_[static_cast<size_t>(pad_wizard_i_)] != control) {
            pad_wizard_i_ = index_of(pad_wizard_, control);
            pad_view_->set_wizard(pad_wizard_i_ + 1, static_cast<int>(pad_wizard_.size()));
        }
        return;
    }
    pad_notify("Czekam na przycisk kierownicy dla: " + PAD_BUTTON_LABELS.at(control));
}

void PanelApp::on_pad_clear(const std::string& control) {
    cfg_.pad.buttons[control] = -1;
    cfg_.normalize();
    pad_view_->set_assignments(cfg_.pad.buttons);
    enqueue_log("info", "Wyczyszczono przypisanie: " + PAD_BUTTON_LABELS.at(control));
}

void PanelApp::pad_defaults() {
    cfg_.pad.buttons = DEFAULT_PAD_BUTTONS;
    cfg_.pad.axes = DEFAULT_PAD_AXES;
    cfg_.normalize();
    pad_view_->set_assignments(cfg_.pad.buttons);
    sync_pad_axis_labels();
    enqueue_log("ok", "Pad: przywrócono domyślne przypisania (skręt na lewym drążku, gaz na RT, hamulec na LT)");
}

void PanelApp::pad_clear_all() {
    for (const auto& n : PAD_BUTTON_NAMES) cfg_.pad.buttons[n] = -1;
    for (const auto& d : PAD_DIRECTIONS) cfg_.pad.axes[d] = "";
    cfg_.normalize();
    pad_view_->set_assignments(cfg_.pad.buttons);
    sync_pad_axis_labels();
    enqueue_log("info", "Wyczyszczono wszystkie przypisania pada");
}

void PanelApp::pad_capture(uint32_t mask) {
    auto control = pad_view_->learning();
    if (!control) {
        pad_prev_buttons_ = mask;
        return;
    }
    // podglad na zywo - widac, ze plytka w ogole cos wysyla
    std::vector<int> held;
    for (int i = 0; i < protocol::NUM_BUTTONS; ++i)
        if ((mask >> i) & 1) held.push_back(i);
    pad_view_->set_held_buttons(held);

    uint32_t fresh = mask & ~pad_prev_buttons_;
    pad_prev_buttons_ = mask;
    if (!fresh) return;
    int index = 0;
    while (!((fresh >> index) & 1)) ++index;

    // ten sam przycisk nie moze sterowac dwiema kontrolkami naraz
    std::vector<std::string> stolen;
    for (auto& [name, assigned] : cfg_.pad.buttons)
        if (assigned == index && name != *control) {
            assigned = -1;
            stolen.push_back(PAD_BUTTON_LABELS.at(name));
        }
    cfg_.pad.buttons[*control] = index;
    cfg_.normalize();
    pad_view_->set_assignments(cfg_.pad.buttons);

    const std::string& label = PAD_BUTTON_LABELS.at(*control);
    std::string note = stolen.empty() ? "" : "  (zabrany z: " + join(stolen, ", ") + ")";
    pad_notify(std::format("Przypisano przycisk {} do {}{}", index, label, note));
    enqueue_log("ok", std::format("Pad: przycisk {} -> {}{}", index, label, note));

    if (!pad_wizard_.empty()) pad_wizard_advance(+1);
    else pad_view_->cancel_learning();
}

void PanelApp::pad_wizard_start() {
    pages_->select(TAB_PAD);
    pad_wizard_ = PAD_WIZARD_ORDER;
    pad_wizard_i_ = 0;
    pad_tools_->set_visible(false);
    pad_wizard_tools_->set_visible(true);
    enqueue_log("info", "Pad: kreator przypisań - wciskaj kolejne przyciski kierownicy, Esc kończy");
    pad_wizard_show();
}

void PanelApp::pad_wizard_show() {
    if (pad_wizard_.empty()) return;
    if (pad_wizard_i_ < 0 || pad_wizard_i_ >= static_cast<int>(pad_wizard_.size())) {
        pad_wizard_stop(true);
        return;
    }
    pad_view_->set_wizard(pad_wizard_i_ + 1, static_cast<int>(pad_wizard_.size()));
    pad_view_->start_learning(pad_wizard_[static_cast<size_t>(pad_wizard_i_)]);
    // zeby przycisk trzymany z poprzedniego kroku nie wpadl od razu do nastepnej
    auto state = engine_->last_state();
    pad_prev_buttons_ = state ? state->buttons_effective : 0;
}

void PanelApp::pad_wizard_advance(int delta) {
    if (pad_wizard_.empty()) return;
    pad_wizard_i_ += delta;
    pad_wizard_show();
}

void PanelApp::pad_wizard_clear() {
    if (pad_wizard_.empty()) return;
    cfg_.pad.buttons[pad_wizard_[static_cast<size_t>(pad_wizard_i_)]] = -1;
    cfg_.normalize();
    pad_view_->set_assignments(cfg_.pad.buttons);
    pad_wizard_advance(+1);
}

void PanelApp::pad_wizard_stop(bool done) {
    if (pad_wizard_.empty()) return;
    pad_wizard_.clear();
    pad_view_->set_wizard(std::nullopt, std::nullopt);
    pad_view_->cancel_learning();
    pad_wizard_tools_->set_visible(false);
    pad_tools_->set_visible(true);
    int filled = 0;
    for (const auto& n : PAD_BUTTON_NAMES)
        if (cfg_.pad.buttons.at(n) >= 0) ++filled;
    pad_notify(std::format("Kreator zakończony - przypisanych kontrolek: {} z {}", filled, PAD_BUTTON_NAMES.size()), 6.0);
    enqueue_log(done ? "ok" : "info", std::format("Pad: kreator zakończony ({} z {} kontrolek przypisanych)", filled, PAD_BUTTON_NAMES.size()));
}

std::vector<std::string> PanelApp::pad_axis_warnings() const {
    const PadConfig& pad = cfg_.pad;
    std::vector<std::string> out;
    for (const auto& d : PAD_DIRECTIONS) {
        std::string source = pad.source_of(d);
        if (!source.empty() && !pad_source_available(source))
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

void PanelApp::reopen_output(bool show_errors) {
    // Zmiana trybu nie moze czekac na nastepna klatke - open_output czyta live().
    engine_->publish_config();
    bool ok = engine_->open_output(show_errors);
    if (!ok && show_errors) {
        std::string msg = engine_->output_error();
        if (msg.empty()) msg = engine_->pad_error();
        if (!msg.empty()) inform("Wyjście", msg, MB_ICONWARNING);
    }
    if (cfg_.ffb.enabled) engine_->set_ffb_enabled(true);
    update_vjoy_text();
}

void PanelApp::update_vjoy_text() {
    VJoyDiagnostics info = vjoy_diagnostics();
    std::vector<std::string> lines;
    lines.push_back(std::format("Panel: {}-bit", info.bits));
    lines.push_back("Biblioteka: " + info.dll_path.value_or("nie znaleziono"));
    if (info.error) {
        lines.push_back("");
        lines.push_back(*info.error);
    } else {
        if (info.version && *info.version)
            lines.push_back(std::format("Wersja vJoy: {:x}.{:02x}", (*info.version >> 8) & 0xFF, *info.version & 0xFF));
        if (info.product) lines.push_back("Produkt: " + *info.product);
        std::string enabled = info.enabled ? "tak" : "NIE";
        if (!info.enabled && cfg_.output.vjoy_wanted() == false) enabled = "nie - wyłączony przez panel (tryb bez vJoy)";
        lines.push_back("Sterownik włączony: " + enabled);
        if (info.devices.empty() && info.enabled) {
            lines.push_back("");
            lines.push_back("Brak włączonych urządzeń vJoy. Uruchom vJoyConf, zaznacz Device 1 i kliknij Apply.");
        }
        for (const VJoyDeviceInfo& d : info.devices) {
            lines.push_back("");
            lines.push_back(std::format("Urządzenie #{}: {}", d.id, d.status_text));
            lines.push_back("  osie      : " + (d.axes.empty() ? std::string("brak") : join(d.axes, ", ")));
            lines.push_back(std::format("  przyciski : {}", d.buttons));
            lines.push_back(std::format("  POV       : {} dyskr. / {} ciągł.", d.disc_pov, d.cont_pov));
            lines.push_back(std::string("  FFB       : ") + (d.ffb ? "tak" : "nie"));
        }
    }
    lines.push_back("");
    lines.push_back("Zalecana konfiguracja dla kierownicy (vJoyConf):");
    lines.push_back("  osie X, Y, Z, Rx, Ry, Rz   32 przyciski   1x POV");
    lines.push_back("  zaznaczone 'Enable Effects' (Force Feedback)");

    // --- tryb pada ---
    ViGEmDiagnostics vg = vigem_diagnostics();
    lines.push_back("");
    lines.push_back(std::string(46, '-'));
    lines.push_back("TRYB PADA (ViGEm)");
    lines.push_back("  biblioteka : " + vg.dll_path.value_or("nie znaleziono"));
    lines.push_back(std::string("  załadowana : ") + (vg.dll_loaded ? "tak" : "NIE"));
    lines.push_back(std::string("  sterownik  : ") + (vg.driver_ok ? "działa" : "NIE DZIAŁA"));
    if (vg.error) lines.push_back("  błąd       : " + *vg.error);
    std::optional<int> slot;
    if (engine_->pad_open(slot))
        lines.push_back("  pad        : podłączony" + (slot ? std::format(", gniazdo XInput {}", *slot + 1) : std::string()));
    else if (cfg_.output.pad_on())
        lines.push_back("  pad        : nie wystartował");
    if (!vg.driver_ok) {
        lines.push_back("");
        lines.push_back("  Zainstaluj ViGEmBus:");
        lines.push_back("  https://github.com/nefarius/ViGEmBus/releases");
    }
    txt_vjoy_->set_lines(lines, TEXT_DIM);
    // Pole rosnie razem z trescia - czesc o padzie nie chowa sie pod spodem.
    txt_vjoy_->set_rows(std::max(9, std::min(static_cast<int>(lines.size()) + 1, 32)));
}

void PanelApp::clear_log() { txt_log_->clear(); }

void PanelApp::copy_log() {
    auto hello = engine_->hello();
    std::string text = std::format("{} {}, tryb {}, firmware {}, C++ x64\n", APP_TITLE, APP_VERSION, cfg_.output.mode, hello ? hello->firmware() : "-") + txt_log_->joined();
    if (hwnd_ && OpenClipboard(hwnd_)) {
        std::wstring wide = widen(text);
        EmptyClipboard();
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
        if (mem) {
            memcpy(GlobalLock(mem), wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
        }
        CloseClipboard();
    }
    lbl_log_copied_->set("✓ Skopiowano");
    log_copied_until_ = monotonic() + 3.0;
}

// polozenie okna i zakladka to nie ustawienia - bez "niezapisanych"
std::string PanelApp::config_snapshot() const {
    AppConfig copy = cfg_;
    copy.ui = UiConfig{};
    return copy.to_json();
}

void PanelApp::save_config() {
    store_ui_state();
    std::filesystem::path path;
    try {
        path = cfg_.save();
    } catch (const std::exception& exc) {
        inform("Zapis ustawień", exc.what(), MB_ICONERROR);
        return;
    }
    saved_snapshot_ = config_snapshot();
    enqueue_log("ok", "Ustawienia zapisane: " + narrow(path.wstring()));
    // Wpis w dzienniku widac tylko w Diagnostyce - tu potwierdzenie stoi obok
    // przycisku, ktory sie kliknelo.
    saved_flash_until_ = monotonic() + SAVED_FLASH_S;
    lbl_saved_->set("✓ Zapisano " + time_stamp(), GOOD);
}

void PanelApp::refresh_saved_label() {
    double now = monotonic();
    if (now < saved_flash_until_ || now - dirty_check_t_ < 0.5) return;
    dirty_check_t_ = now;
    bool dirty = config_snapshot() != saved_snapshot_;
    lbl_saved_->set(dirty ? DIRTY_HINT : SAVE_HINT, dirty ? WARN : MUTED);
}

void PanelApp::enqueue_log(const std::string& level, const std::string& msg) {
    std::lock_guard lock(log_mutex_);
    if (log_queue_.size() < 2000) log_queue_.emplace_back(level, msg);
}

void PanelApp::drain_log() {
    if (!txt_log_) return;
    std::lock_guard lock(log_mutex_);
    for (int i = 0; i < 60 && !log_queue_.empty(); ++i) {
        auto [level, msg] = std::move(log_queue_.front());
        log_queue_.pop_front();
        txt_log_->append(time_stamp() + "  " + msg, level_color(level));
    }
}

// --------------------------------------------------------------- odswiezanie

std::string PanelApp::g29_state(bool connected, const EngineStatus& st) const {
    if (!connected) return "offline";
    if (!st.g29_fw) return "old_fw";
    if (!st.g29_attached) return cfg_.output.g29_wanted() ? "attaching" : "hidden";
    if (!st.g29_usb) {
        // Po ponownym podlaczeniu Windows wylicza G29 1-2 s - bez tego przez
        // chwile wisialo "podlacz kabel", choc kabel jest
        if (monotonic() - st.g29_attached_since < G29_DETECT_S) return "detecting";
        return "no_usb";
    }
    return "ok";
}

void PanelApp::refresh_g29_card(bool connected, const EngineStatus& st) {
    if (!connected) {
        for (auto& [key, row] : g29_rows_) row->set("-", MUTED);
        lbl_g29_report_->set("raport: -");
        return;
    }
    g29_rows_["fw"]->set(st.g29_fw ? "tak" : std::string("nie - wgraj firmware ") + protocol::FW_LATEST_TEXT, st.g29_fw ? GOOD : BAD);
    static const std::map<std::string, std::pair<std::string, RGB>> usb_rows = {
        {"offline", {"-", MUTED}}, {"old_fw", {"-", MUTED}}, {"hidden", {"nie - ukryta, tryb bez G29", MUTED}},
        {"attaching", {"podłączanie…", WARN}}, {"detecting", {"Windows ją wykrywa…", WARN}},
        {"no_usb", {"nie - podłącz kabel do Native USB", BAD}}, {"ok", {"tak", GOOD}}};
    const auto& usb = usb_rows.at(g29_state(connected, st));
    g29_rows_["usb"]->set(usb.first, usb.second);
    if (!cfg_.output.g29_on()) g29_rows_["feed"]->set("wyłączone - inny tryb wyjścia", MUTED);
    else if (st.g29_fed) g29_rows_["feed"]->set("docierają", GOOD);
    else g29_rows_["feed"]->set("brak", WARN);
    std::string game = std::to_string(st.g29_out_packets);
    if (st.g29_game_range) game += std::format("  (zakres {} st.)", st.g29_game_range);
    g29_rows_["game"]->set(game);
    auto rep = cfg_.output.g29_on() ? engine_->last_g29() : std::nullopt;
    lbl_g29_report_->set(rep ? "raport: " + hex_bytes(rep->data(), rep->size()) : std::string("raport: -"));
}

void PanelApp::refresh_title(LinkState state, const std::string& port) {
    std::string status;
    if (flash_proc_) status = "wgrywanie firmware";
    else if (state == LinkState::Connected) status = port.empty() ? "połączono" : "połączono (" + port + ")";
    else if (state == LinkState::Scanning) status = "szukanie płytki";
    else status = "rozłączono";
    std::string title = std::format("{} {} · {}", APP_TITLE, APP_VERSION, status);
    if (title != title_) {
        title_ = title;
        if (hwnd_) SetWindowTextW(hwnd_, widen(title).c_str());
    }
}

std::pair<std::string, std::string> PanelApp::output_buttons_text(const ProcessedState& s) const {
    std::vector<int> sorted(s.out_buttons.begin(), s.out_buttons.begin() + s.out_buttons_count);
    std::sort(sorted.begin(), sorted.end());
    std::string out_txt;
    for (int b : sorted) out_txt += (out_txt.empty() ? "" : ", ") + std::to_string(b);
    if (out_txt.empty()) out_txt = "nic";
    std::string hat_txt = s.hat < 0 ? "" : std::string("   krzyżak: ") + HAT_DIRECTIONS[static_cast<size_t>(s.hat)];
    if (cfg_.output.g29_on()) {
        // G29 ma wlasne przypisania - lista vJoy dotyczy tylko vJoy
        auto report = engine_->last_g29();
        auto act = g29_active(report ? *report : build_g29_report(s, cfg_));
        std::vector<std::string> names;
        for (const auto& n : G29_CONTROLS)
            if (act[n]) names.push_back(G29_CONTROL_LABELS.at(n));
        std::string g29_txt = names.empty() ? "nic" : join(names, ", ");
        if (cfg_.output.vjoy_buttons_only())
            return {std::format("Wysyłane do vJoy: {}{}   |   do G29: {}", out_txt, hat_txt, g29_txt), std::format("Do G29: {}\nDo vJoy: {}{}", g29_txt, out_txt, hat_txt)};
        return {"Tryb G29 - przypisania są w zakładce G29. Wciśnięte na G29: " + g29_txt, "Do G29: " + g29_txt};
    }
    return {"Wysyłane do vJoy: " + out_txt + hat_txt, "Do vJoy: " + out_txt + hat_txt};
}

std::vector<std::string> PanelApp::axis_conflicts() const {
    std::vector<std::pair<std::string, std::vector<std::string>>> used;
    auto add = [&](const std::string& axis, const std::string& who) {
        for (auto& [a, names] : used)
            if (a == axis) {
                names.push_back(who);
                return;
            }
        used.push_back({axis, {who}});
    };
    if (cfg_.steer.vjoy_axis != "-") add(cfg_.steer.vjoy_axis, "kierownica");
    for (size_t i = 0; i < NUM_PEDALS; ++i) {
        const PedalConfig& p = cfg_.pedals[i];
        if (p.vjoy_axis != "-" && p.source >= 0) add(p.vjoy_axis, PEDAL_LABELS.at(PEDAL_KEYS[i]));
    }
    std::vector<std::string> out;
    for (const auto& [axis, names] : used)
        if (names.size() > 1) out.push_back(std::format("{} ({})", vjoy_axis_label(axis), join(names, ", ")));
    return out;
}

std::string PanelApp::output_hint(bool connected, bool vjoy_ok, bool pad_ok, const EngineStatus& st) const {
    const OutputConfig& out = cfg_.output;
    std::string g29 = g29_state(connected, st);
    if (out.g29_on() && g29 == "old_fw") return "Firmware płytki nie ma trybu G29 - wgraj nowy (Diagnostyka → Wgraj firmware).";
    if (out.g29_on() && g29 == "no_usb") return "Windows nie widzi G29 - podłącz drugi kabel do portu Native USB płytki.";
    if (st.vjoy_switching) {
        auto wanted = out.vjoy_wanted();
        return std::string(!wanted ? "Przełączam" : (*wanted ? "Włączam" : "Wyłączam")) + " vJoy - Windows pyta o zgodę administratora (okno UAC).";
    }
    std::string pad_error = engine_->pad_error();
    if (out.pad_on() && !pad_ok && !pad_error.empty()) return first_line(pad_error);
    if (out.wheel_on() && !vjoy_ok) {
        if (!st.vjoy_switch_error.empty()) return "Nie udało się włączyć vJoy (" + st.vjoy_switch_error + "). Ponów: Diagnostyka → Podłącz ponownie.";
        std::string output_error = engine_->output_error();
        if (out.vjoy_buttons_only()) {
            std::string text = "vJoy nie działa - gry widzą G29, ale bez dodatkowych przycisków z vJoy. " + first_line(output_error);
            while (!text.empty() && text.back() == ' ') text.pop_back();
            return text;
        }
        return output_error.empty() ? "vJoy nie jest aktywne - gry nie widzą kierownicy. Sprawdź zakładkę Diagnostyka." : output_error;
    }
    if (out.vjoy_wanted() == false && !st.vjoy_switch_error.empty())
        return "Nie udało się ukryć vJoy (" + st.vjoy_switch_error + ") - gry widzą je obok kierownicy. Ponów: Diagnostyka → Podłącz ponownie.";
    // w trybie G29 + vJoy osie vJoy stoja na srodku - konflikty nie maja znaczenia
    if (!out.vjoy_buttons_only()) {
        auto conflicts = axis_conflicts();
        if (!conflicts.empty()) return "Ta sama oś vJoy jest przypisana do kilku źródeł: " + join(conflicts, ", ");
    }
    auto hello = engine_->hello();
    if (connected && hello && hello->outdated())
        return std::format("Płytka ma firmware {}, a w firmware/kierownica czeka {} - wgraj go (Diagnostyka → Wgraj firmware), żeby mieć wszystkie funkcje panelu.",
                           hello->firmware(), protocol::FW_LATEST_TEXT);
    return "";
}

std::string PanelApp::ffb_state_text(bool connected, const EngineStatus& st) const {
    if (!cfg_.ffb.enabled) return "Wyłączone - silnik jest zwolniony.";
    std::string txt;
    const FfbManager& ffb = engine_->ffb();
    if (ffb.registered()) {
        txt = std::format("Aktywne. Pakietów z gry: {}, wzmocnienie urządzenia: {:.0f} %, efekty: {}", ffb.packets.load(), ffb.device_gain() * 100, ffb.active_effects().size());
        if (ffb.parse_errors.load()) txt += std::format(", odrzucone pakiety: {}", ffb.parse_errors.load());
    } else {
        txt = "Włączone, ale bez odbioru z gry - działają tylko efekty lokalne (sprężyna, tłumienie, tarcie).";
    }
    if (!st.board_has_ffb && connected) txt += "  UWAGA: firmware zgłosił brak obsługi FFB.";
    return txt;
}

void PanelApp::refresh_pad_tab(const std::optional<ProcessedState>& state, bool pad_ok, const EngineStatus& st) {
    pad_view_->set_assignments(cfg_.pad.buttons);
    sync_pad_axis_labels();
    std::optional<PadState> pad_state;
    if (state) {
        pad_state = engine_->last_pad();
        if (!pad_state) pad_state = build_pad_state(*state, cfg_);
    }
    if (pad_state) {
        std::map<std::string, bool> active;
        std::vector<std::string> names;
        for (const auto& n : PAD_BUTTON_NAMES) {
            bool on = (pad_state->buttons & xusb_mask(n)) != 0;
            active[n] = on;
            if (on) names.push_back(PAD_BUTTON_LABELS.at(n));
        }
        pad_view_->set_active(active);
        pad_view_->set_triggers(pad_state->lt, pad_state->rt);
        pad_view_->set_sticks(pad_state->lx / 32767.0, pad_state->ly / 32767.0, pad_state->rx / 32767.0, pad_state->ry / 32767.0);
        if (!pad_view_->learning() && monotonic() >= pad_notice_until_)
            lbl_pad_live_->set("Aktywne na padzie: " + (names.empty() ? std::string("nic") : join(names, ", ")), GOOD);
    } else {
        pad_view_->set_active({});
        pad_view_->set_triggers(0, 0);
        pad_view_->set_sticks(0, 0, 0, 0);
        if (!pad_view_->learning() && monotonic() >= pad_notice_until_) lbl_pad_live_->set(OFFLINE_LIVE, MUTED);
    }
    auto warnings = pad_axis_warnings();
    if (!warnings.empty()) {
        std::string text;
        for (const auto& w : warnings) text += (text.empty() ? "• " : "\n• ") + w;
        lbl_pad_conflict_->set(text);
        lbl_pad_conflict_->set_visible(true);
    } else {
        lbl_pad_conflict_->set_visible(false);
    }
    std::string text;
    if (cfg_.output.pad_on()) {
        std::string err = engine_->pad_error();
        text = pad_ok ? st.pad_text : (err.empty() ? "Pad nie wystartował" : first_line(err));
    } else {
        text = "Tryb pada wyłączony - obok wybierz 'Pad' albo 'Oba naraz', żeby gry zobaczyły kontroler.";
    }
    lbl_pad_state_->set(text);
}

void PanelApp::refresh() {
    engine_->publish_config();
    drain_log();
    refresh_saved_label();
    if (log_copied_until_ != 0.0 && monotonic() >= log_copied_until_) {
        log_copied_until_ = 0;
        lbl_log_copied_->set("");
    }

    EngineStatus st = engine_->status();
    if (vjoy_was_switching_ && !st.vjoy_switching) update_vjoy_text();  // stan sterownika zmienil sie w tle
    vjoy_was_switching_ = st.vjoy_switching;

    LinkState state = engine_->link().state();
    LinkStats stats = engine_->link_stats();
    bool connected = state == LinkState::Connected;
    int tab = pages_->current();

    // --- naglowek ---
    poll_flash();
    refresh_title(state, stats.port);
    if (was_connected_ && (state == LinkState::Scanning || state == LinkState::Error)) {
        // Plytka zniknela sama (nie "Rozlacz") - przy grze na pierwszym planie
        // nikt nie patrzy na panel, wiec mruga pasek zadan.
        if (!flash_proc_ && !winsys::is_foreground(hwnd_)) winsys::flash_taskbar(hwnd_);
    }
    was_connected_ = connected;
    auto set_connect = [this](const char* text, Button::Style style) {
        btn_connect_->set(text);
        btn_connect_->set_style(style);
    };
    if (flash_proc_) {
        dot_link_->set(WARN);
        lbl_link_->set("wgrywanie firmware…", WARN);
        set_connect("Połącz", Button::Accent);
    } else if (connected) {
        dot_link_->set(GOOD);
        lbl_link_->set(std::format("{} - {:.0f} Hz", stats.port, stats.frames_per_sec), GOOD);
        set_connect("Rozłącz", Button::Normal);
    } else if (state == LinkState::Scanning) {
        dot_link_->set(WARN);
        lbl_link_->set(st.link_detail.empty() ? "szukanie" : st.link_detail, WARN);
        set_connect("Przerwij", Button::Normal);
    } else {
        dot_link_->set(MUTED);
        lbl_link_->set(link_state_label(state), TEXT_DIM);
        set_connect("Połącz", Button::Accent);
    }

    bool vjoy_ok = st.output_ok, pad_ok = st.pad_ok;
    if (cfg_.output.mode == "none") {
        dot_vjoy_->set(MUTED);
        lbl_vjoy_->set("podgląd", TEXT_DIM);
    } else {
        // level: 0 dziala, 1 w trakcie (np. okno UAC), 2 blad
        std::vector<std::string> parts;
        int level = 0;
        if (cfg_.output.wheel_on()) {
            if (vjoy_ok) {
                parts.push_back(std::format("vJoy #{}", cfg_.output.vjoy_device));
            } else if (st.vjoy_switching) {
                parts.push_back("vJoy: włączanie");
                level = std::max(level, 1);
            } else {
                parts.push_back("vJoy błąd");
                level = 2;
            }
        }
        if (cfg_.output.pad_on()) {
            parts.push_back(pad_ok ? "pad" : "pad błąd");
            if (!pad_ok) level = 2;
        }
        if (cfg_.output.g29_on()) {
            static const std::map<std::string, std::pair<std::string, int>> g29_text = {
                {"offline", {"G29: brak płytki", 2}}, {"old_fw", {"G29: stary firmware", 2}}, {"attaching", {"G29: podłączanie", 1}},
                {"detecting", {"G29: wykrywanie", 1}}, {"no_usb", {"G29: podłącz Native USB", 2}}, {"ok", {"G29", 0}}};
            auto it = g29_text.find(g29_state(connected, st));
            parts.push_back(it == g29_text.end() ? "G29" : it->second.first);
            if (it != g29_text.end()) level = std::max(level, it->second.second);
        }
        RGB color = std::array<RGB, 3>{GOOD, WARN, BAD}[static_cast<size_t>(level)];
        dot_vjoy_->set(color);
        lbl_vjoy_->set(join(parts, " + "), color);
    }

    auto state_obj = engine_->last_state();
    bool live = state_obj.has_value() && connected;

    // --- pulpit ---
    if (tab == TAB_DASH) {
        if (live) {
            dial_->set_angle(state_obj->steer_deg);
            row_counts_->set(std::format("{:+d}", state_obj->raw_counts));
            row_axis_->set(std::format("{:+.3f}", state_obj->steer_axis));
            row_velocity_->set(std::format("{:+.0f} st/s", state_obj->steer_velocity));
            for (size_t i = 0; i < NUM_PEDALS; ++i) {
                int src = cfg_.pedals[i].source;
                std::optional<int> raw = (src >= 0 && src < protocol::NUM_AXES) ? std::optional<int>(state_obj->raw_axes[static_cast<size_t>(src)]) : std::nullopt;
                dash_bars_[i]->set(state_obj->pedals[i], raw, src >= 0);
            }
            // Pokazujemy stan PO odwroceniu (NC).
            button_grid_->set_mask(state_obj->buttons_effective);
            hat_indicator_->set(state_obj->hat);
            lbl_dash_out_->set(output_buttons_text(*state_obj).second);
        } else if (!connected) {
            dial_->set_angle(0.0);
            button_grid_->set_mask(0);
            hat_indicator_->set(-1);
            lbl_dash_out_->set("");
            for (size_t i = 0; i < NUM_PEDALS; ++i) dash_bars_[i]->set(0.0, std::nullopt, cfg_.pedals[i].source >= 0);
            row_counts_->set("-");
            row_axis_->set("-");
            row_velocity_->set("-");
        }

        std::vector<std::string> out_lines;
        if (cfg_.output.g29_on()) out_lines.push_back(st.output_text);
        if (cfg_.output.wheel_on()) {
            // bez dzialajacego vJoy silnik ma podglad - jego opis "gry nie widza
            // kierownicy" klamal w trybie G29 + vJoy
            if (vjoy_ok) out_lines.push_back(engine_->output_status_text());
            else if (st.vjoy_switching) out_lines.push_back("vJoy - czekam na włączenie sterownika");
            else out_lines.push_back("vJoy nieaktywne");
        }
        if (cfg_.output.pad_on()) out_lines.push_back(st.pad_text);
        if (out_lines.empty()) out_lines.push_back("Tryb podglądu - gry nie widzą kierownicy");
        lbl_output_->set(join(out_lines, "\n"));
        std::string hint = output_hint(connected, vjoy_ok, pad_ok, st);
        lbl_output_hint_->set(hint);
        lbl_output_hint_->set_visible(!hint.empty());
        torque_meter_->set(st.ffb_torque);

        for (auto& [deg, btn] : range_buttons_) btn->set_style(deg == cfg_.steer.range_deg ? Button::Accent : Button::Normal);
    }

    // --- przyciski ---
    if (tab == TAB_BUTTONS) {
        uint32_t mask = live ? state_obj->buttons_effective : 0;
        for (size_t i = 0; i < btn_dots_.size(); ++i) {
            bool on = (mask >> i) & 1;
            btn_dots_[i]->set(on ? ACCENT : TRACK, on);
        }
        if (live) {
            std::vector<std::string> pressed;
            for (int i = 0; i < protocol::NUM_BUTTONS; ++i)
                if ((mask >> i) & 1) pressed.push_back(std::to_string(i));
            lbl_btn_live_->set("Wciśnięte teraz: " + (pressed.empty() ? std::string("brak") : join(pressed, ", ")));
            lbl_btn_out_->set(output_buttons_text(*state_obj).first, GOOD);
        } else {
            lbl_btn_live_->set("Wciśnięte teraz: -");
            lbl_btn_out_->set(OFFLINE_LIVE, MUTED);
        }
    }

    // --- force feedback ---
    if (tab == TAB_FFB) {
        ffb_meter_->set(st.ffb_torque);
        lbl_ffb_state_->set(ffb_state_text(connected, st));
    }

    // --- diagnostyka ---
    if (tab == TAB_DIAG) {
        diag_rows_["port"]->set(stats.port.empty() ? "-" : stats.port);
        auto hello = engine_->hello();
        if (!hello) diag_rows_["fw"]->set("-", TEXT);
        else if (hello->outdated()) diag_rows_["fw"]->set(std::format("{} {} (nowszy: {})", hello->name, hello->firmware(), protocol::FW_LATEST_TEXT), WARN);
        else diag_rows_["fw"]->set(hello->name + " " + hello->firmware(), TEXT);
        diag_rows_["fps"]->set(std::format("{:.0f} Hz", stats.frames_per_sec), stats.frames_per_sec > 50 ? GOOD : WARN);
        diag_rows_["bps"]->set(std::format("{:.1f} kB/s", stats.bytes_per_sec / 1024));
        diag_rows_["lat"]->set(std::format("{:.1f} ms", stats.latency_ms), stats.latency_ms < 15 ? GOOD : WARN);
        diag_rows_["crc"]->set(std::to_string(stats.crc_errors), stats.crc_errors == 0 ? GOOD : BAD);
        diag_rows_["drop"]->set(std::to_string(stats.dropped_frames), stats.dropped_frames == 0 ? GOOD : WARN);
        diag_rows_["resync"]->set(std::to_string(stats.resyncs));
        diag_rows_["total"]->set(std::to_string(stats.total_frames));
        diag_rows_["up"]->set(fmt_duration(stats.uptime));
        // wykresy maja stala os czasu - jeden punkt na ~33 ms
        double now = monotonic();
        if (now - spark_t_ >= 0.033) {
            spark_t_ = now;
            spark_fps_->push(stats.frames_per_sec, 520);
            spark_lat_->push(stats.latency_ms, 40);
        }
        refresh_g29_card(connected, st);
    }

    // --- pedaly: podglad zakresow ---
    if (tab == TAB_PEDALS) {
        RawSeen seen = engine_->raw_seen();
        for (size_t i = 0; i < NUM_PEDALS; ++i) {
            const PedalConfig c = cfg_.pedals[i];
            PedalWidgets& w = pedal_widgets_[i];
            w.range->set(std::format("{} - {}", c.raw_min, c.raw_max));
            if (c.source >= 0 && c.source < protocol::NUM_AXES) {
                int lo = seen.min[static_cast<size_t>(c.source)], hi = seen.max[static_cast<size_t>(c.source)];
                w.seen->set(hi >= lo ? std::format("{} - {}", lo, hi) : std::string("-"));
            } else {
                w.seen->set("kanał wyłączony");
            }
            std::optional<int> raw;
            if (live) {
                if (c.source >= 0 && c.source < protocol::NUM_AXES) raw = state_obj->raw_axes[static_cast<size_t>(c.source)];
                w.bar->set(state_obj->pedals[i], raw);
            }
            std::string key = std::format("{}/{}/{}/{}", c.deadzone_low, c.deadzone_high, c.gamma, i);
            w.plot->set_curve(key, [c](double x) { return pedal_curve(x, c); },
                              {{0.0, c.deadzone_low}, {1.0 - c.deadzone_high, 1.0}});
            if (raw) {
                double pos = pedal_position(*raw, c);
                w.plot->set_marker(std::make_pair(pos, pedal_curve(pos, c)));
            } else {
                w.plot->set_marker(std::nullopt);
            }
        }
    }

    // --- kierownica: wykres liniowosci ---
    if (tab == TAB_STEER) {
        double lin = cfg_.steer.linearity;
        double half = std::max(1.0, cfg_.steer.range_deg / 2.0);
        steer_plot_->set_curve(std::to_string(lin), [lin](double x) { return steer_curve(x, lin); });
        steer_plot_->set_captions(std::format("-{:.0f}°", half), "kąt kierownicy → oś w grze", std::format("+{:.0f}°", half));
        if (live) {
            double x = state_obj->steer_deg / half;
            steer_plot_->set_marker(std::make_pair(x, steer_curve(x, lin)));
        } else {
            steer_plot_->set_marker(std::nullopt);
        }
    }

    // --- zakladka G29 ---
    g29tab_->tick(state_obj, connected, tab == TAB_G29);

    // --- zakladka Pad ---
    if (live) pad_capture(state_obj->buttons_effective);
    pad_view_->tick();
    if (tab == TAB_PAD) refresh_pad_tab(live ? state_obj : std::nullopt, pad_ok, st);  // bez lacza nie pokazujemy ostatniej ramki

    // --- pasek stanu ---
    std::vector<std::string> flags;
    if (live) {
        if (state_obj->flags & protocol::FLAG_ENCODER_ERROR) flags.push_back("błędy kwadratury enkodera");
        if (state_obj->flags & protocol::FLAG_ENDSTOP_HIT) flags.push_back("na ograniczniku");
        if (state_obj->flags & protocol::FLAG_FFB_WATCHDOG) flags.push_back("watchdog FFB zadziałał");
    }
    lbl_status_->set("Konfiguracja: " + narrow(config_path().wstring()) + (flags.empty() ? "" : "   |   " + join(flags, ", ")), flags.empty() ? MUTED : WARN);
}

// ------------------------------------------------------------------- okno

void PanelApp::layout_root() {
    Ui& ui = Ui::get();
    float w = std::max(1.0f, ui.window.w - 32), x = 16, y = 14;
    float bottom = ui.window.h - 14;
    float header_h = header_->measure(w).h;
    header_->arrange({x, y, w, header_h});
    y += header_h + 14;
    float tab_h = tabbar_->measure(w).h;
    tabbar_->arrange({x, y, w, tab_h});
    y += tab_h + 1 + 12;
    float status_h = statusbar_->measure(w).h;
    statusbar_->arrange({x, bottom - status_h, w, status_h});
    pages_->arrange({x, y, w, std::max(1.0f, bottom - status_h - 23 - y)});
    if (Widget* o = ui.overlay()) o->arrange({0, 0, ui.window.w, ui.window.h});
    ui.layout_dirty = false;
}

void PanelApp::render_to(ID2D1RenderTarget* rt) {
    Gfx& g = Gfx::get();
    g.set_target(rt);
    Ui& ui = Ui::get();
    if (ui.layout_dirty) layout_root();
    rt->BeginDraw();
    g.clear(BG);
    header_->paint(g);
    tabbar_->paint(g);
    float rule_y = tabbar_->r.bottom();
    g.fill_rect({16, rule_y, ui.window.w - 32, 1}, BORDER);
    pages_->paint(g);
    g.fill_rect({16, statusbar_->r.y - 11, ui.window.w - 32, 1}, BORDER);
    statusbar_->paint(g);
    if (Widget* o = ui.overlay()) o->paint(g);
}

void PanelApp::create_target() {
    Gfx& g = Gfx::get();
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = static_cast<float>(dpi_);
    target_.Reset();
    g.factory()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)), &target_);
    g.drop_resources();
    if (target_) g.set_target(target_.Get());
}

void PanelApp::render() {
    if (!target_) create_target();
    if (!target_) return;
    render_to(target_.Get());
    HRESULT hr = target_->EndDraw();
    Ui::get().dirty = false;
    if (hr == D2DERR_RECREATE_TARGET) {
        target_.Reset();
        Gfx::get().drop_resources();
        Ui::get().invalidate();
    }
}

void PanelApp::request_paint() {
    // zamknieta nakladka moze byc jeszcze zapamietana jako widget pod kursorem
    if (Ui::get().in_trash(hover_)) hover_ = nullptr;
    if (Ui::get().in_trash(pressed_)) pressed_ = nullptr;
    Ui::get().collect_garbage();
    if (hwnd_ && Ui::get().dirty) InvalidateRect(hwnd_, nullptr, FALSE);
}

bool PanelApp::create_window() {
    HINSTANCE inst = GetModuleHandleW(nullptr);

    // Prosta ikona rysowana w locie - brak zewnetrznych plikow.
    const int n = 32;
    std::vector<uint32_t> pixels(static_cast<size_t>(n * n), 0);
    auto put = [&](int x, int y) { if (x >= 0 && y >= 0 && x < n && y < n) pixels[static_cast<size_t>(y * n + x)] = 0xFF000000u | ACCENT; };
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double d = std::hypot(x - 15.5, y - 15.5);
            if ((d >= 11.5 && d <= 15.0) || d <= 3.5) put(x, y);
        }
    for (int t = 4; t < 12; ++t) {
        put(15, 15 - t);
        put(15 - t, 15 + t / 2);
        put(15 + t, 15 + t / 2);
    }
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (bits) memcpy(bits, pixels.data(), pixels.size() * 4);
    HBITMAP mask = CreateBitmap(n, n, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = winsys::WINDOW_CLASS;
    RegisterClassExW(&wc);

    std::wstring title = widen(title_);
    hwnd_ = CreateWindowExW(0, winsys::WINDOW_CLASS, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1240, 880, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    return true;
}

// Okno tam, gdzie bylo przy zamknieciu, a pierwszy raz - na srodku. Rozmiar
// przycinamy do monitora: na malym laptopie okno nie moze byc wieksze niz pulpit.
void PanelApp::place_window() {
    float s = scale();
    int min_w = static_cast<int>(MIN_W * s), min_h = static_cast<int>(MIN_H * s);
    DWORD style = WS_OVERLAPPEDWINDOW;
    auto apply = [&](int w, int h, int x, int y) {
        RECT rc{0, 0, w, h};
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi_);
        SetWindowPos(hwnd_, nullptr, x, y, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    if (auto saved = winsys::parse_geometry(cfg_.ui.geometry)) {
        if (auto work = winsys::work_area_at(saved->x, saved->y, saved->w)) {
            int w = std::max(min_w, std::min(saved->w, (*work)[2] - (*work)[0]));
            int h = std::max(min_h, std::min(saved->h, (*work)[3] - (*work)[1]));
            normal_geometry_ = std::format("{}x{}+{}+{}", w, h, saved->x, saved->y);
            apply(w, h, saved->x, saved->y);
            return;
        }
    }
    int screen_w = GetSystemMetrics(SM_CXSCREEN), screen_h = GetSystemMetrics(SM_CYSCREEN);
    int w = std::max(min_w, std::min(static_cast<int>(1240 * s), screen_w - 80));
    int h = std::max(min_h, std::min(static_cast<int>(880 * s), screen_h - 120));
    apply(w, h, std::max(0, (screen_w - w) / 2), std::max(0, (screen_h - h) / 3));
}

void PanelApp::remember_geometry() {
    if (!hwnd_ || IsIconic(hwnd_)) return;
    zoomed_ = IsZoomed(hwnd_) != FALSE;
    if (zoomed_) return;  // zmaksymalizowane podaje rozmiar ekranu
    RECT window, client;
    GetWindowRect(hwnd_, &window);
    GetClientRect(hwnd_, &client);
    normal_geometry_ = std::format("{}x{}+{}+{}", client.right, client.bottom, window.left, window.top);
}

void PanelApp::store_ui_state() {
    if (!normal_geometry_.empty()) cfg_.ui.geometry = normal_geometry_;
    cfg_.ui.zoomed = zoomed_;
    cfg_.ui.tab = pages_->current();
}

void PanelApp::on_close() {
    if (closing_) return;
    closing_ = true;
    if (hwnd_) KillTimer(hwnd_, TIMER_ID);
    store_ui_state();
    try {
        cfg_.save();
    } catch (const std::exception&) {
    }
    engine_->stop();
    timeEndPeriod(1);
    if (hwnd_) DestroyWindow(hwnd_);
}

void PanelApp::set_timer_period(UINT ms) {
    if (ms != timer_period_) {
        timer_period_ = ms;
        SetTimer(hwnd_, TIMER_ID, ms, nullptr);
    }
}

void PanelApp::tick() {
    if (closing_) return;
    try {
        refresh();
    } catch (const std::exception& exc) {  // interfejs nie moze przerwac petli
        enqueue_log("error", std::string("Błąd odświeżania: ") + exc.what());
    }
    // Zminimalizowane albo w calosci zasloniete okno odswiezamy rzadko - w
    // trakcie gry panel nie zabiera jej procesora na rysowanie.
    double now = monotonic();
    if (hidden_ || now - hidden_t_ >= HIDDEN_CHECK_S) {
        hidden_t_ = now;
        hidden_ = winsys::window_hidden(hwnd_);
    }
    set_timer_period((IsIconic(hwnd_) || hidden_) ? UI_IDLE_PERIOD_MS : UI_PERIOD_MS);
    request_paint();
}

Widget* PanelApp::widget_at(float x, float y) {
    Ui& ui = Ui::get();
    if (Widget* o = ui.overlay()) return o->hit_test(x, y);
    if (Widget* w = header_->hit_test(x, y)) return w;
    if (tabbar_->r.contains(x, y)) return tabbar_.get();
    if (Widget* w = pages_->hit_test(x, y)) return w;
    return statusbar_->hit_test(x, y);
}

void PanelApp::update_hover(float x, float y) {
    if (pressed_) return;
    Widget* w = widget_at(x, y);
    if (w != hover_) {
        if (hover_) hover_->mouse_leave();
        hover_ = w;
        if (hover_) hover_->mouse_enter();
    }
}

Pt PanelApp::to_dip(LPARAM lp) const {
    return {static_cast<float>(GET_X_LPARAM(lp)) / scale(), static_cast<float>(GET_Y_LPARAM(lp)) / scale()};
}

LRESULT PanelApp::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Ui& ui = Ui::get();
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        render();
        return 0;
    }
    case WM_SIZE: {
        float w = static_cast<float>(LOWORD(lp)) / scale(), h = static_cast<float>(HIWORD(lp)) / scale();
        if (wp != SIZE_MINIMIZED && LOWORD(lp) > 0) {
            ui.window = {w, h};
            if (target_) target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            ui.relayout();
        }
        remember_geometry();
        return 0;
    }
    case WM_MOVE:
        remember_geometry();
        return 0;
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        if (target_) target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        Gfx::get().drop_resources();
        const RECT* rc = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ui.relayout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        RECT rc{0, 0, static_cast<LONG>(MIN_W * scale()), static_cast<LONG>(MIN_H * scale())};
        AdjustWindowRectExForDpi(&rc, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi_);
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize.x = rc.right - rc.left;
        info->ptMinTrackSize.y = rc.bottom - rc.top;
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ID) tick();
        return 0;
    case WM_MOUSEMOVE: {
        Pt p = to_dip(lp);
        ui.mouse = p;
        update_hover(p.x, p.y);
        Widget* target = pressed_ ? pressed_ : hover_;
        if (target) target->mouse_move(p.x, p.y);
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);
        request_paint();
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!pressed_ && hover_) {
            hover_->mouse_leave();
            hover_ = nullptr;
            request_paint();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        Pt p = to_dip(lp);
        update_hover(p.x, p.y);
        Widget* w = hover_;
        if (ui.focus() && ui.focus() != w) ui.set_focus(nullptr);
        if (w) {
            pressed_ = w;
            SetCapture(hwnd);
            w->mouse_down(p.x, p.y);
        }
        request_paint();
        return 0;
    }
    case WM_LBUTTONUP: {
        Pt p = to_dip(lp);
        Widget* w = pressed_;
        pressed_ = nullptr;
        ReleaseCapture();
        if (w) w->mouse_up(p.x, p.y);
        hover_ = nullptr;
        update_hover(p.x, p.y);
        request_paint();
        return 0;
    }
    case WM_RBUTTONDOWN: {
        Pt p = to_dip(lp);
        update_hover(p.x, p.y);
        if (hover_) hover_->mouse_rdown(p.x, p.y);
        request_paint();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        float x = static_cast<float>(pt.x) / scale(), y = static_cast<float>(pt.y) / scale();
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (Widget* o = ui.overlay()) {
            o->mouse_wheel(delta);
        } else {
            // Kolko nad comboboxem nie zmienia wartosci - przewija strone.
            Widget* w = widget_at(x, y);
            if (!(w && w->mouse_wheel(delta))) {
                if (Rect pr = pages_->r; pr.contains(x, y)) {
                    // strona pod kursorem - dziecko Pages to Scroll
                    float probe_x = pr.right() - 2;
                    if (Widget* s = pages_->hit_test(probe_x, y)) s->mouse_wheel(delta);
                }
            }
        }
        request_paint();
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            Widget* w = hover_;
            SetCursor(LoadCursorW(nullptr, (w && w->hand_cursor()) ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && wp == 'S') {
            save_config();
        } else if (ctrl && wp >= '1' && wp < '1' + TAB_COUNT) {
            go_tab(static_cast<int>(wp - '1'));
        } else if (wp == VK_ESCAPE) {
            on_escape();
        } else if (Widget* o = ui.overlay()) {
            o->key_down(static_cast<unsigned>(wp));
        } else if (ui.focus()) {
            ui.focus()->key_down(static_cast<unsigned>(wp));
        }
        request_paint();
        return 0;
    }
    case WM_CHAR:
        if (ui.focus() && GetKeyState(VK_CONTROL) >= 0) ui.focus()->char_input(static_cast<wchar_t>(wp));
        request_paint();
        return 0;
    case WM_CLOSE:
        on_close();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int PanelApp::run() {
    if (!create_window()) return 1;
    place_window();
    engine_->set_ui_hwnd(hwnd_);
    build();
    timeBeginPeriod(1);  // zegar 1 ms zamiast 15,6 - rowne klatki podgladu

    ShowWindow(hwnd_, options_.minimized ? SW_SHOWMINNOACTIVE : (cfg_.ui.zoomed ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL));
    {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        Ui::get().window = {static_cast<float>(rc.right) / scale(), static_cast<float>(rc.bottom) / scale()};
        Ui::get().relayout();
    }
    // Okno juz stoi - otwieranie wyjsc (pad czeka do 3 s) nie zaslania startu.
    UpdateWindow(hwnd_);
    engine_->start();
    set_timer_period(UI_PERIOD_MS);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

// ---------------------------------------------------------------- selftest
//
// Bez okna i bez sprzetu: rysuje wszystkie zakladki do PNG i sprawdza zachowanie
// z udawana plytka - odpowiednik testow okna w wersji Python.

namespace {

protocol::StateFrame make_frame(int32_t counts = 0, uint32_t buttons = 0, int axis = 1000, uint8_t flags = 0x80 | 0x20 | 0x40) {
    protocol::StateFrame f;
    f.steer_counts = counts;
    f.buttons = buttons;
    f.axes.fill(static_cast<uint16_t>(axis));
    f.flags = flags;
    f.host_time = monotonic() + 1.0;
    return f;
}

protocol::HelloInfo make_hello() {
    protocol::HelloInfo h;
    h.proto = 1;
    h.fw_major = 2;
    h.fw_minor = 2;
    h.caps = protocol::CAP_FFB | protocol::CAP_G29 | protocol::CAP_G29_HIDE | protocol::CAP_NATIVE_USB;
    h.num_buttons = 32;
    h.num_axes = 6;
    h.report_hz = 333;
    h.name = "Due";
    return h;
}

} // namespace

bool PanelApp::render_png(const fs::path& file) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    UINT w = static_cast<UINT>(Ui::get().window.w), h = static_cast<UINT>(Ui::get().window.h);
    ComPtr<IWICBitmap> bitmap;
    wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    if (!bitmap) return false;

    static ComPtr<ID2D1RenderTarget> shared;  // obrazki w pamieci naleza do jednego celu
    Gfx& g = Gfx::get();
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                                       D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(g.factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return false;
    g.drop_resources();
    shared = rt;
    Ui::get().relayout();
    render_to(rt.Get());
    if (FAILED(rt->EndDraw())) return false;
    Ui::get().dirty = false;

    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
    ComPtr<IWICBitmapEncoder> encoder;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(w, h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&format);
    frame->WriteSource(bitmap.Get(), nullptr);
    frame->Commit();
    return SUCCEEDED(encoder->Commit());
}

bool PanelApp::selftest_checks(std::string& report) {
    bool ok = true;
    auto check = [&](bool cond, const std::string& what) {
        report += (cond ? "ok    " : "BLAD  ") + what + "\n";
        ok = ok && cond;
    };
    auto dirty_text = [this] {
        dirty_check_t_ = 0;
        saved_flash_until_ = 0;
        refresh_saved_label();
        return lbl_saved_->text();
    };
    auto log_has = [this](const char* needle) { return txt_log_->joined().find(needle) != std::string::npos; };

    engine_->link().simulate_connected(make_hello(), "COM9");

    // --- kazda zakladka odswieza sie z plytka ---
    for (int tab = 0; tab < TAB_COUNT; ++tab) {
        go_tab(tab);
        for (int k = 0; k < 4; ++k) {
            engine_->handle_state_for_test(make_frame(k * 400, 1u << k, 1000 + k * 500));
            refresh();
        }
        std::error_code ec;
        fs::create_directories(options_.selftest_dir, ec);
        check(render_png(options_.selftest_dir / widen(std::format("tab_{}_{}.png", tab, TAB_NAMES[static_cast<size_t>(tab)]))), std::format("render zakladki {}", tab));
    }
    drain_log();
    check(!log_has("Błąd odświeżania") && !log_has("Błąd w oknie"), "brak bledow odswiezania w dzienniku");
    check(lbl_vjoy_->text() == "G29", "naglowek: G29 (jest: " + lbl_vjoy_->text() + ")");
    check(title_.find("połączono (COM9)") != std::string::npos, "tytul okna ze stanem lacza");

    // --- tryb wspolny dla wszystkich zakladek ---
    cfg_.output.mode = "g29_vjoy";
    sync_mode_widgets();
    bool same = true;
    for (Combo* c : mode_combos_) same = same && c->text() == OUTPUT_MODE_LABELS.at("g29_vjoy");
    check(same && mode_combos_.size() == 4, "wybor trybu wspolny dla zakladek");
    cfg_.output.mode = "g29";
    sync_mode_widgets();
    engine_->publish_config();

    // --- wskaznik niezapisanych zmian i zapis stanu okna ---
    check(dirty_text() == SAVE_HINT, "swieza konfiguracja bez niezapisanych zmian");
    go_tab(TAB_BUTTONS);
    check(dirty_text() == SAVE_HINT, "zakladka to nie ustawienie");
    pedal_widgets_[1].gamma->apply(1.5);
    check(dirty_text() == DIRTY_HINT, "zmiana suwaka to niezapisana zmiana");
    save_config();
    {
        json::Value saved = json::parse([&] {
            std::ifstream f(config_path(), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(f), {});
        }());
        check(std::abs(saved.find("pedals")->find("brake")->find("gamma")->to_double() - 1.5) < 1e-9, "zapis wartosci suwaka");
        check(saved.find("ui")->find("tab")->to_int(-1) == TAB_BUTTONS, "zapis zakladki");
    }
    check(dirty_text() == SAVE_HINT, "po zapisie brak niezapisanych zmian");

    // --- suwaki maja te same granice co normalize() ---
    go_tab(TAB_PEDALS);
    refresh();
    layout_root();
    PedalWidgets& w = pedal_widgets_[0];
    w.dz_lo->apply(w.dz_lo->value_at(10000));
    check(std::abs(cfg_.pedals[0].deadzone_low - 0.45) < 1e-6, "suwak martwej strefy do konca = 0.45");
    w.gamma->apply(w.gamma->value_at(10000));
    check(std::abs(cfg_.pedals[0].gamma - 3.0) < 1e-6, "suwak gamma do konca = 3.0");
    cfg_.ffb.min_duty = 200;
    cfg_.normalize();
    check(cfg_.ffb.max_duty > cfg_.ffb.min_duty, "PWM max > PWM min");
    cfg_.pedals[0].deadzone_low = 0.02;
    cfg_.pedals[0].gamma = 1.0;
    sync_pedal_widgets();

    // --- stany G29 w naglowku ---
    go_tab(TAB_DASH);
    engine_->handle_state_for_test(make_frame(0, 0, 1000, 0x40));  // odlaczona
    refresh();
    check(lbl_vjoy_->text() == "G29: podłączanie", "G29 odlaczona = podlaczanie (jest: " + lbl_vjoy_->text() + ")");
    engine_->handle_state_for_test(make_frame(0, 0, 1000, 0x80 | 0x40));  // Windows jeszcze liczy
    refresh();
    check(lbl_vjoy_->text() == "G29: wykrywanie", "G29 podlaczona bez USB = wykrywanie (jest: " + lbl_vjoy_->text() + ")");

    // --- nauka przycisku pada ---
    go_tab(TAB_PAD);
    engine_->handle_state_for_test(make_frame());
    refresh();
    pad_view_->start_learning("A");
    check(pad_view_->canvas()->visual_state("A") == "learn_on", "nauka: kontrolka miga");
    engine_->handle_state_for_test(make_frame(0, 1u << 20));
    refresh();
    check(cfg_.pad.buttons["A"] == 20 && !pad_view_->learning(), "nauka: przycisk 20 trafia do A");

    // przycisk nie moze sterowac dwiema kontrolkami
    engine_->handle_state_for_test(make_frame());
    refresh();
    pad_view_->start_learning("B");
    engine_->handle_state_for_test(make_frame(0, 1u << 20));
    refresh();
    check(cfg_.pad.buttons["B"] == 20 && cfg_.pad.buttons["A"] == -1, "nauka: przycisk zabrany z poprzedniej kontrolki");

    // kreator przechodzi kolejne kroki i konczy sie na Esc
    engine_->handle_state_for_test(make_frame());
    refresh();
    pad_wizard_start();
    check(pad_view_->learning() == std::optional<std::string>("A"), "kreator: pierwszy krok A");
    engine_->handle_state_for_test(make_frame(0, 1u << 2));
    refresh();
    check(pad_view_->learning() == std::optional<std::string>("B"), "kreator: po przypisaniu krok B");
    check(render_png(options_.selftest_dir / "pad_kreator.png"), "render kreatora pada");
    on_escape();
    check(!pad_view_->learning() && pad_wizard_.empty(), "kreator: Esc konczy");

    // menu zrodel na osi
    on_pad_axis_click("LT", 300, 200);
    check(Ui::get().overlay() != nullptr, "menu zrodla osi otwarte");
    check(render_png(options_.selftest_dir / "pad_menu.png"), "render menu");
    Ui::get().close_overlay();
    Ui::get().collect_garbage();
    set_pad_axis_source("LT", "clutch");
    check(cfg_.pad.axes["LT"] == "clutch", "zrodlo osi zapisane");
    pad_defaults();

    // --- G29: nauka i przepisywanie z pada ---
    go_tab(TAB_G29);
    engine_->handle_state_for_test(make_frame());
    refresh();
    g29tab_->view->start_learning("GEAR_R");
    check(pages_->current() == TAB_G29, "G29: zostaje na zakladce");
    engine_->handle_state_for_test(make_frame(0, 1u << 30));
    refresh();
    check(cfg_.g29.buttons["GEAR_R"] == 30, "G29: nauka biegu wstecznego");
    g29tab_->copy_from_pad();
    check(cfg_.g29.buttons["GEAR_R"] == 30, "G29: przepisanie z pada zostawia biegi");
    check(render_png(options_.selftest_dir / "g29_po_nauce.png"), "render G29 po nauce");

    // --- rysunek G29: kontrolki nie nachodza na siebie i mieszcza sie w plotnie ---
    bool in_bounds = true;
    const auto& parts = g29tab_->view->canvas()->parts();
    for (const Part& part : parts) {
        float x1, y1, x2, y2;
        part.shape.bbox(x1, y1, x2, y2);
        in_bounds = in_bounds && x1 >= 0 && y1 >= 0 && x2 <= 486 && y2 <= 388;
    }
    check(in_bounds && parts.size() == G29_CONTROLS.size(), "G29: kontrolki w granicach plotna");
    bool overlap = false;
    for (float y = 0.5f; y < 388 && !overlap; y += 0.5f)
        for (float x = 0.5f; x < 486 && !overlap; x += 0.5f)
            if (g29tab_->view->canvas()->parts_at(x, y).size() > 1) overlap = true;
    check(!overlap, "G29: kontrolki nie nachodza na siebie");

    // --- rysunek pada: spusty i bumpery po stronach, klik w tlo nic nie uczy ---
    bool lb = false, a_btn = false;
    for (const Part& part : pad_view_->canvas()->parts()) {
        lb = lb || part.name == "LB";
        a_btn = a_btn || part.name == "A";
    }
    check(lb && a_btn, "pad: czesci rysunku utworzone");

    // --- przy odlaczonej plytce podglad gasnie ---
    engine_->link().disconnect();
    engine_->handle_state_for_test(make_frame());
    return ok;
}

int PanelApp::selftest() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::error_code ec;
    fs::create_directories(options_.selftest_dir, ec);
    cfg_.link.auto_connect = false;
    cfg_.output.auto_acquire = false;
    saved_snapshot_ = config_snapshot();

    Ui::get().window = {1240, 880};
    // Pierwszy cel, zeby budowa drzewa mogla mierzyc napisy.
    build();
    engine_->start();

    std::string report;
    bool ok = false;
    try {
        ok = selftest_checks(report);
    } catch (const std::exception& exc) {
        report += std::string("BLAD  wyjatek: ") + exc.what() + "\n";
    }
    drain_log();
    report += "\n--- dziennik ---\n" + txt_log_->joined() + "\n";
    report += ok ? "\nWYNIK: OK\n" : "\nWYNIK: BLEDY\n";
    std::ofstream(options_.selftest_dir / "selftest.log", std::ios::binary) << report;
    engine_->stop();
    return ok ? 0 : 1;
}

} // namespace wb::ui

#include "app.h"

#include <dwmapi.h>
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
#include "../util.h"
#include "../winsys.h"

namespace fs = std::filesystem;

namespace wb::ui {

namespace {

constexpr double SAVED_CHECK_S = 0.5;
constexpr UINT UI_PERIOD_MS = 16;         // ok. 60 klatek na sekunde
constexpr UINT UI_IDLE_PERIOD_MS = 200;   // okno niewidoczne - podgladu i tak nie widac
constexpr double G29_DETECT_S = 5.0;      // tyle Windows ma na wyliczenie ponownie podlaczonej G29
constexpr double HIDDEN_CHECK_S = 0.25;   // co ile sprawdzamy, czy okno jest zasloniete
constexpr double TOOLTIP_DELAY_S = 0.45;
constexpr UINT_PTR TIMER_ID = 1;
constexpr int MIN_W = 940, MIN_H = 600;
constexpr float WIDE_SIDEBAR_FROM = 1320.0f;

struct TabInfo {
    const char* name;
    const char* subtitle;
    const char* icon;
    const char* group;
};
const std::array<TabInfo, TAB_COUNT> TABS = {{
    {"Pulpit", "Stan kierownicy i wyjścia do gier", "", "Jazda"},
    {"Kierownica", "Zakres obrotu, enkoder i charakterystyka", "", "Sprzęt"},
    {"Pedały", "Kalibracja, martwe strefy i krzywe", "", "Sprzęt"},
    {"Przyciski", "Mapowanie na vJoy, krzyżak i styki NC", "", "Sprzęt"},
    {"G29", "Przyciski kierownicy Logitech G29", "", "Wyjście"},
    {"Pad", "Wirtualny kontroler Xbox 360", "", "Wyjście"},
    {"Force Feedback", "Moment, efekty i bezpieczeństwo", "", "Wyjście"},
    {"Diagnostyka", "Łącze, sterowniki i dziennik", "", "System"},
}};

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

std::string time_stamp() {
    std::time_t t = std::time(nullptr);
    std::tm local{};
    localtime_s(&local, &t);
    return std::format("{:02d}:{:02d}:{:02d}", local.tm_hour, local.tm_min, local.tm_sec);
}

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

    g_app = this;
    Gfx::get().init();
    enqueue_log("info", "Konfiguracja: " + narrow(config_path().wstring()));
    if (!load_problem.empty()) enqueue_log("warn", load_problem);
}

PanelApp::~PanelApp() { g_app = nullptr; }

void PanelApp::log(const std::string& level, const std::string& msg) { enqueue_log(level, msg); }

void PanelApp::build() {
    std::vector<Sidebar::Item> nav;
    for (const TabInfo& t : TABS) nav.push_back({t.name, t.icon, t.group});
    sidebar_ = mk<Sidebar>(std::move(nav), [this](int i) { go_tab(i); });
    topbar_ = mk<TopBar>([this](int c) { on_caption(c); });

    link_chip_ = topbar_->actions->add(mk<Chip>("Rozłączono", MUTED, [this] { open_link_popover(); }, true), Pack{.cross = Pack::Center});
    btn_connect_ = topbar_->actions->add(mk<Button>("Połącz", Button::Accent, [this] { toggle_connect(); }), Pack{.left = 8, .cross = Pack::Center});
    output_chip_ = topbar_->actions->add(mk<Chip>("-", MUTED, [this] { open_mode_popover(output_chip_->r); }, true), Pack{.left = 8, .cross = Pack::Center});
    btn_save_ = topbar_->actions->add(mk<Button>("Zapisano", Button::Ghost, [this] { save_config(); }, 104.0f), Pack{.left = 8, .cross = Pack::Center});
    btn_save_->set_icon(ic::check);
    btn_save_->set_tooltip("Zapisz ustawienia (Ctrl+S)");

    pages_ = mk<Pages>();
    pages_->on_changed = [this](int i) { on_tab_changed(i); };

    auto add_page = [this](Tab tab, auto page) {
        auto* raw = page.get();
        page_list_[tab] = std::move(page);
        return raw;
    };
    add_page(TAB_DASH, mk<HomePage>(*this));
    add_page(TAB_STEER, mk<WheelPage>(*this));
    pedals_ = add_page(TAB_PEDALS, mk<PedalsPage>(*this));
    buttons_ = add_page(TAB_BUTTONS, mk<ButtonsPage>(*this));
    g29_ = add_page(TAB_G29, mk<G29Page>(*this));
    pad_ = add_page(TAB_PAD, mk<PadPage>(*this));
    add_page(TAB_FFB, mk<FfbPage>(*this));
    diag_ = add_page(TAB_DIAG, mk<DiagPage>(*this));

    for (auto& page : page_list_) pages_->add(mk<Scroll>(page->build(), 26.0f, 6.0f));

    refresh_ports();
    diag_->update_vjoy_text();
    topbar_->set_title(TABS[0].name, TABS[0].subtitle);
    if (cfg_.ui.tab > 0 && cfg_.ui.tab < TAB_COUNT) go_tab(cfg_.ui.tab);
}

void PanelApp::open_link_popover() {
    refresh_ports();
    auto box = mk<Box>(false);
    box->add(mk<Label>("ŁĄCZE Z PŁYTKĄ", Font::Caps, MUTED), Pack{.bottom = 10});

    auto row = mk<Box>(true);
    row->add(mk<Combo>(port_labels_, port_index_, [this](int i) { port_index_ = i; }, 30), Pack{.flex = 1, .cross = Pack::Center});
    row->add(mk<IconButton>(ic::refresh, "Odśwież listę portów", [this] {
        Ui::get().close_overlay();
        open_link_popover();
    }), Pack{.left = 6, .cross = Pack::Center});
    box->add(std::move(row));

    LinkState state = engine_->link().state();
    if (auto hello = engine_->hello()) {
        LinkStats stats = engine_->link_stats();
        box->add(mk<Label>(std::format("{} · firmware {} · {:.0f} Hz · opóźnienie {:.1f} ms", hello->name, hello->firmware(), stats.frames_per_sec, stats.latency_ms), Font::Small, TEXT_DIM, 2000.0f), Pack{.top = 10});
    }

    bool active = state == LinkState::Connected || state == LinkState::Scanning;
    auto button = mk<Button>(state == LinkState::Connected ? "Rozłącz" : (state == LinkState::Scanning ? "Przerwij" : "Połącz"), active ? Button::Normal : Button::Accent, [this] {
        Ui::get().close_overlay();
        toggle_connect();
    });
    box->add(std::move(button), Pack{.top = 12});
    Ui::get().open_overlay(mk<Popover>(std::move(box), link_chip_->r, 400.0f));
}

void PanelApp::open_mode_popover(const Rect& anchor) {
    auto box = mk<Box>(false);
    box->add(mk<Label>("TRYB WYJŚCIA", Font::Caps, MUTED), Pack{.bottom = 8});
    auto grid = mk<Grid>(std::vector<float>{1, 1});
    for (size_t i = 0; i < MODE_INFO.size(); ++i) {
        const ModeInfo& m = MODE_INFO[i];
        auto tile = mk<ModeTile>(m.icon, m.title, m.desc, [this, key = std::string(m.key)] {
            Ui::get().close_overlay();
            set_mode(key);
        });
        tile->set_selected(cfg_.output.mode == m.key);
        grid->place(std::move(tile), static_cast<int>(i) / 2, static_cast<int>(i) % 2, 1, Pack{.left = 3, .top = 3, .right = 3, .bottom = 3});
    }
    box->add(std::move(grid));
    Ui::get().open_overlay(mk<Popover>(std::move(box), anchor, 460.0f));
}

// --------------------------------------------------------------- polecenia

void PanelApp::go_tab(int index) { pages_->select(index); }

void PanelApp::on_tab_changed(int index) {
    sidebar_->select(index);
    topbar_->set_title(TABS[static_cast<size_t>(index)].name, TABS[static_cast<size_t>(index)].subtitle);
    // Nauka trwala dalej na innej zakladce: przycisk wcisniety np. na Pulpicie
    // po cichu trafial do pada.
    if (index != TAB_G29) g29_->stop();
    if (index != TAB_PAD) pad_->stop_learning();
    if (index == TAB_DIAG) unread_alerts_ = 0;
}

void PanelApp::on_escape() {
    if (Ui::get().overlay()) {
        Ui::get().close_overlay();
        return;
    }
    if (pages_->current() == TAB_G29) g29_->escape();
    else pad_->escape();
}

void PanelApp::refresh_ports() {
    port_labels_ = {"Automatycznie (wykryj płytkę)"};
    port_devices_ = {""};
    port_index_ = 0;
    for (const PortInfo& p : list_ports()) {
        port_labels_.push_back(p.label());
        port_devices_.push_back(p.device);
        if (!p.device.empty() && p.device == cfg_.link.port) port_index_ = static_cast<int>(port_labels_.size()) - 1;
    }
}

std::string PanelApp::selected_port() const {
    return (port_index_ >= 0 && port_index_ < static_cast<int>(port_devices_.size())) ? port_devices_[static_cast<size_t>(port_index_)] : std::string();
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

bool PanelApp::firmware_available() const {
    std::error_code ec;
    return fs::is_regular_file(firmware_script(), ec);
}

void PanelApp::flash_firmware() {
    if (flash_proc_) return;
    fs::path script = firmware_script();
    if (!firmware_available()) {
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
    diag_->set_flash_enabled(false);
    std::string where = port.empty() ? "(skrypt sam znajdzie port)" : "przez " + port;
    enqueue_log("info", "Wgrywanie firmware " + where + " - panel połączy się po zamknięciu okna wgrywania");
}

void PanelApp::poll_flash() {
    if (!flash_proc_ || WaitForSingleObject(flash_proc_, 0) != WAIT_OBJECT_0) return;
    CloseHandle(flash_proc_);
    flash_proc_ = nullptr;
    diag_->set_flash_enabled(true);
    enqueue_log("info", "Okno wgrywania zamknięte");
    if (flash_reconnect_) engine_->connect();
}

void PanelApp::reset_board() {
    if (!engine_->is_connected()) {
        // bez lacza reset nigdzie nie pojdzie - nie wylaczamy tez FFB
        inform("Reset płytki", "Brak połączenia z płytką.", MB_ICONINFORMATION);
        return;
    }
    if (ask("Reset płytki", "Wysłać reset do Arduino?\n\nForce Feedback zostanie wyłączony, a środek kierownicy ustawiony na bieżącą pozycję.")) {
        engine_->reset_board();
        cfg_.ffb.enabled = false;
    }
}

void PanelApp::set_range(int degrees) {
    cfg_.steer.range_deg = degrees;
    engine_->apply_config();
    enqueue_log("ok", std::format("Zakres obrotu: {} stopni", degrees));
}

void PanelApp::set_mode(const std::string& mode) {
    cfg_.output.mode = mode;
    reopen_output();
    enqueue_log("info", "Tryb wyjścia: " + OUTPUT_MODE_LABELS.at(cfg_.output.mode));
    Ui::get().toast("Tryb wyjścia: " + OUTPUT_MODE_LABELS.at(cfg_.output.mode), ACCENT, ic::link);
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
    diag_->update_vjoy_text();
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
    Ui::get().toast("Ustawienia zapisane " + time_stamp(), GOOD, ic::save);
    dirty_check_t_ = 0;
    refresh_save_button();
}

void PanelApp::refresh_save_button() {
    double now = monotonic();
    if (now - dirty_check_t_ < SAVED_CHECK_S) return;
    dirty_check_t_ = now;
    dirty_ = config_snapshot() != saved_snapshot_;
    btn_save_->set(dirty_ ? "Zapisz" : "Zapisano");
    btn_save_->set_style(dirty_ ? Button::Accent : Button::Ghost);
    btn_save_->set_icon(dirty_ ? ic::save : ic::check);
}

void PanelApp::enqueue_log(const std::string& level, const std::string& msg) {
    std::lock_guard lock(log_mutex_);
    if (log_queue_.size() < 2000) log_queue_.emplace_back(level, msg);
}

void PanelApp::drain_log() {
    std::lock_guard lock(log_mutex_);
    for (int i = 0; i < 60 && !log_queue_.empty(); ++i) {
        auto [level, msg] = std::move(log_queue_.front());
        log_queue_.pop_front();
        diag_->append_log(time_stamp() + "  " + msg, level);
        if ((level == "warn" || level == "error") && pages_->current() != TAB_DIAG) ++unread_alerts_;
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

std::pair<std::string, int> PanelApp::output_summary(bool connected, const EngineStatus& st) const {
    if (cfg_.output.mode == "none") return {"podgląd", 0};
    // level: 0 dziala, 1 w trakcie (np. okno UAC), 2 blad
    std::vector<std::string> parts;
    int level = 0;
    if (cfg_.output.wheel_on()) {
        if (st.output_ok) {
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
        parts.push_back(st.pad_ok ? "pad" : "pad błąd");
        if (!st.pad_ok) level = 2;
    }
    if (cfg_.output.g29_on()) {
        static const std::map<std::string, std::pair<std::string, int>> g29_text = {
            {"offline", {"G29: brak płytki", 2}}, {"old_fw", {"G29: stary firmware", 2}}, {"attaching", {"G29: podłączanie", 1}},
            {"detecting", {"G29: wykrywanie", 1}}, {"no_usb", {"G29: podłącz Native USB", 2}}, {"ok", {"G29", 0}}};
        auto it = g29_text.find(g29_state(connected, st));
        parts.push_back(it == g29_text.end() ? "G29" : it->second.first);
        if (it != g29_text.end()) level = std::max(level, it->second.second);
    }
    return {join(parts, " + "), level};
}

std::pair<std::string, std::string> PanelApp::output_buttons_text(const ProcessedState& s) const {
    static const std::array<const char*, 4> hat_names = {"góra", "prawo", "dół", "lewo"};
    std::vector<int> sorted(s.out_buttons.begin(), s.out_buttons.begin() + s.out_buttons_count);
    std::sort(sorted.begin(), sorted.end());
    std::string out_txt;
    for (int b : sorted) out_txt += (out_txt.empty() ? "" : ", ") + std::to_string(b);
    if (out_txt.empty()) out_txt = "nic";
    std::string hat_txt = s.hat < 0 ? "" : std::string("   krzyżak: ") + hat_names[static_cast<size_t>(s.hat)];
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

std::string PanelApp::output_hint(bool connected, const EngineStatus& st) const {
    const OutputConfig& out = cfg_.output;
    bool vjoy_ok = st.output_ok, pad_ok = st.pad_ok;
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

void PanelApp::refresh_shell(const Frame& f) {
    const EngineStatus& st = f.st;
    if (flash_proc_) {
        link_chip_->set("wgrywanie firmware…", WARN);
        link_chip_->set_spinner(true);
    } else if (f.connected) {
        link_chip_->set(std::format("{} · {:.0f} Hz", f.stats.port, f.stats.frames_per_sec), GOOD);
        link_chip_->set_spinner(false);
    } else if (f.link == LinkState::Scanning) {
        link_chip_->set(st.link_detail.empty() ? "szukanie" : st.link_detail, WARN);
        link_chip_->set_spinner(true);
    } else {
        link_chip_->set(link_state_label(f.link), MUTED);
        link_chip_->set_spinner(false);
    }
    // jedno klikniecie, gdy nic nie jest polaczone; reszta w panelu lacza
    btn_connect_->set_visible(!flash_proc_ && !f.connected && f.link != LinkState::Scanning);

    auto [summary, level] = output_summary(f.connected, st);
    RGB color = cfg_.output.mode == "none" ? MUTED : std::array<RGB, 3>{GOOD, WARN, BAD}[static_cast<size_t>(level)];
    output_chip_->set(summary, color);
    output_chip_->set_spinner(level == 1);

    const ProcessedState* s = f.live ? &*f.state : nullptr;
    sidebar_->set_cluster(f.live, s ? s->steer_deg : 0.0, cfg_.steer.range_deg, s ? s->pedals[0] : 0.0, s ? s->pedals[1] : 0.0, s ? s->pedals[2] : 0.0);

    // znaczki w nawigacji: co wymaga uwagi, widac z kazdej zakladki
    bool board_flags = s && (s->flags & (protocol::FLAG_ENCODER_ERROR | protocol::FLAG_ENDSTOP_HIT | protocol::FLAG_FFB_WATCHDOG));
    sidebar_->set_badge(TAB_DASH, board_flags ? "!" : "", WARN);  // szczegoly na Pulpicie
    sidebar_->set_badge(TAB_FFB, cfg_.ffb.enabled ? "ON" : "", GOOD);
    std::string g29 = g29_state(f.connected, st);
    bool g29_bad = cfg_.output.g29_on() && (g29 == "old_fw" || g29 == "no_usb");
    sidebar_->set_badge(TAB_G29, g29_bad ? "!" : "", BAD);
    sidebar_->set_badge(TAB_DIAG, unread_alerts_ > 0 ? std::to_string(std::min(unread_alerts_, 99)) : "", WARN);
}

void PanelApp::refresh() {
    engine_->publish_config();
    drain_log();
    refresh_save_button();

    Frame f;
    f.st = engine_->status();
    f.stats = engine_->link_stats();
    f.link = engine_->link().state();
    f.connected = f.link == LinkState::Connected;
    f.state = engine_->last_state();
    f.live = f.state.has_value() && f.connected;
    f.now = monotonic();

    if (vjoy_was_switching_ && !f.st.vjoy_switching) diag_->update_vjoy_text();  // stan sterownika zmienil sie w tle
    vjoy_was_switching_ = f.st.vjoy_switching;

    poll_flash();
    refresh_title(f.link, f.stats.port);
    if (was_connected_ && (f.link == LinkState::Scanning || f.link == LinkState::Error)) {
        // Plytka zniknela sama (nie "Rozlacz") - przy grze na pierwszym planie
        // nikt nie patrzy na panel, wiec mruga pasek zadan.
        if (!flash_proc_ && !winsys::is_foreground(hwnd_)) winsys::flash_taskbar(hwnd_);
        Ui::get().toast("Płytka przestała odpowiadać", BAD, ic::warn);
    }
    if (!was_connected_ && f.connected) Ui::get().toast("Połączono z płytką na " + f.stats.port, GOOD, ic::usb);
    was_connected_ = f.connected;

    refresh_shell(f);
    int tab = pages_->current();
    for (int i = 0; i < TAB_COUNT; ++i) page_list_[static_cast<size_t>(i)]->refresh(f, i == tab);
}

// ------------------------------------------------------------------- okno

void PanelApp::layout_root() {
    Ui& ui = Ui::get();
    sidebar_->set_compact(ui.window.w < WIDE_SIDEBAR_FROM);
    float side = sidebar_->width();
    sidebar_->arrange({0, 0, side, ui.window.h});
    topbar_->arrange({side, 0, std::max(1.0f, ui.window.w - side), TopBar::HEIGHT});
    pages_->arrange({side, TopBar::HEIGHT, std::max(1.0f, ui.window.w - side), std::max(1.0f, ui.window.h - TopBar::HEIGHT)});
    ui.arrange_overlays();
    ui.layout_dirty = false;
}

void PanelApp::render_to(ID2D1RenderTarget* rt) {
    Gfx& g = Gfx::get();
    g.set_target(rt);
    Ui& ui = Ui::get();
    if (ui.layout_dirty) layout_root();
    rt->BeginDraw();
    g.clear(BG);
    g.backdrop(sidebar_->width(), 0);
    pages_->paint(g);
    topbar_->paint(g);
    sidebar_->paint(g);
    ui.paint_toasts(g);
    ui.paint_overlays(g);
    ui.paint_tooltip(g);
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
    Ui& ui = Ui::get();
    double now = monotonic();
    ui.dt = last_render_ != 0.0 ? static_cast<float>(std::min(now - last_render_, 0.05)) : 0.016f;
    ui.time = now;
    last_render_ = now;
    ui.dirty = false;  // animacje zamawiaja nastepna klatke juz w trakcie rysowania
    render_to(target_.Get());
    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        target_.Reset();
        Gfx::get().drop_resources();
        ui.invalidate();
    }
}

void PanelApp::request_paint() {
    Ui& ui = Ui::get();
    // zamknieta nakladka moze byc jeszcze zapamietana jako widget pod kursorem
    if (ui.in_trash(hover_)) hover_ = nullptr;
    if (ui.in_trash(pressed_)) pressed_ = nullptr;
    ui.collect_garbage();
    if (hwnd_ && ui.dirty) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PanelApp::apply_frame_style() {
    // ciemne ramki systemowe i zaokraglone rogi; tresc rysujemy sami az po krawedz
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    int corners = 2;  // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    COLORREF border = 0x00352a23;
    DwmSetWindowAttribute(hwnd_, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    MARGINS margins{1, 1, 1, 1};  // zachowuje cien systemowy okna
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
    SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
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
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = winsys::WINDOW_CLASS;
    RegisterClassExW(&wc);

    std::wstring title = widen(title_);
    hwnd_ = CreateWindowExW(0, winsys::WINDOW_CLASS, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1400, 900, nullptr, nullptr, inst, nullptr);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    apply_frame_style();
    return true;
}

// Okno tam, gdzie bylo przy zamknieciu, a pierwszy raz - na srodku. Rozmiar
// przycinamy do monitora: na malym laptopie okno nie moze byc wieksze niz pulpit.
void PanelApp::place_window() {
    float s = scale();
    int min_w = static_cast<int>(MIN_W * s), min_h = static_cast<int>(MIN_H * s);
    auto apply = [&](int w, int h, int x, int y) { SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE); };
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
    int w = std::max(min_w, std::min(static_cast<int>(1400 * s), screen_w - 80));
    int h = std::max(min_h, std::min(static_cast<int>(900 * s), screen_h - 120));
    apply(w, h, std::max(0, (screen_w - w) / 2), std::max(0, (screen_h - h) / 3));
}

void PanelApp::remember_geometry() {
    if (!hwnd_ || IsIconic(hwnd_)) return;
    zoomed_ = IsZoomed(hwnd_) != FALSE;
    if (zoomed_) return;  // zmaksymalizowane podaje rozmiar ekranu
    RECT window;
    GetWindowRect(hwnd_, &window);
    normal_geometry_ = std::format("{}x{}+{}+{}", window.right - window.left, window.bottom - window.top, window.left, window.top);
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

void PanelApp::on_caption(int caption) {
    if (caption == 1) ShowWindow(hwnd_, SW_MINIMIZE);
    else if (caption == 2) ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
    else if (caption == 3) on_close();
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
    double now = monotonic();
    Ui& ui = Ui::get();
    ui.tick_toasts(now);
    const std::string* tip = (hover_ && !pressed_ && !ui.overlay() && now - last_move_ > TOOLTIP_DELAY_S) ? hover_->tooltip() : nullptr;
    ui.set_tooltip(tip);

    // Zminimalizowane albo w calosci zasloniete okno odswiezamy rzadko - w
    // trakcie gry panel nie zabiera jej procesora na rysowanie.
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
    if (sidebar_->r.contains(x, y)) return sidebar_.get();
    if (topbar_->r.contains(x, y)) return topbar_->hit_test(x, y);
    return pages_->hit_test(x, y);
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

// Ramka okna jest nasza: krawedzie zmieniaja rozmiar, pusty pasek u gory
// przesuwa okno, a przycisk maksymalizacji daje systemowe Snap Layouts.
LRESULT PanelApp::hit_test_frame(HWND hwnd, LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd, &pt);
    RECT rc;
    GetClientRect(hwnd, &rc);
    float x = static_cast<float>(pt.x) / scale(), y = static_cast<float>(pt.y) / scale();
    if (!IsZoomed(hwnd)) {
        int border = static_cast<int>(6 * scale());
        bool left = pt.x < border, right = pt.x >= rc.right - border, top = pt.y < border, bottom = pt.y >= rc.bottom - border;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (bottom) return HTBOTTOM;
        if (top && !topbar_->caption_at(x, y)) return HTTOP;
    }
    if (Ui::get().overlay()) return HTCLIENT;
    int caption = topbar_->caption_at(x, y);
    if (caption == 2) return HTMAXBUTTON;
    if (caption) return HTCLIENT;
    if (sidebar_->is_drag_area(x, y)) return HTCAPTION;
    if (y < TopBar::HEIGHT && x >= sidebar_->width() && !topbar_->hit_test(x, y)) return HTCAPTION;
    return HTCLIENT;
}

LRESULT PanelApp::handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Ui& ui = Ui::get();
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp == TRUE) {
            if (IsZoomed(hwnd)) {
                // zmaksymalizowane okno wystaje poza ekran o grubosc ramki
                auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi_) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi_) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                params->rgrc[0].left += fx;
                params->rgrc[0].top += fy;
                params->rgrc[0].right -= fx;
                params->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST:
        if (!topbar_) break;
        return hit_test_frame(hwnd, lp);
    case WM_NCACTIVATE:
        return DefWindowProcW(hwnd, msg, wp, -1);
    case WM_NCMOUSEMOVE:
        if (topbar_) {
            topbar_->set_caption_hover(wp == HTMAXBUTTON ? 2 : 0);
            if (wp == HTMAXBUTTON) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                TrackMouseEvent(&tme);
            }
            if (hover_) {
                hover_->mouse_leave();
                hover_ = nullptr;
            }
            request_paint();
        }
        break;
    case WM_NCMOUSELEAVE:
        if (topbar_) {
            topbar_->set_caption_hover(0);
            topbar_->press_caption(0);
            request_paint();
        }
        break;
    case WM_NCLBUTTONDOWN:
        if (wp == HTMAXBUTTON) {
            topbar_->press_caption(2);
            request_paint();
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (wp == HTMAXBUTTON) {
            topbar_->press_caption(0);
            on_caption(2);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (pages_) render();
        return 0;
    }
    case WM_SIZE: {
        float w = static_cast<float>(LOWORD(lp)) / scale(), h = static_cast<float>(HIWORD(lp)) / scale();
        if (wp != SIZE_MINIMIZED && LOWORD(lp) > 0) {
            ui.window = {w, h};
            if (target_) target_->Resize(D2D1::SizeU(LOWORD(lp), HIWORD(lp)));
            if (topbar_) topbar_->set_maximized(wp == SIZE_MAXIMIZED);
            ui.relayout();
            // przy przeciaganiu krawedzi tresc ma nadazac, bez czarnych pasow
            if (pages_) render();
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
        auto* info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize.x = static_cast<LONG>(MIN_W * scale());
        info->ptMinTrackSize.y = static_cast<LONG>(MIN_H * scale());
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_ID) tick();
        return 0;
    case WM_MOUSEMOVE: {
        Pt p = to_dip(lp);
        ui.mouse = p;
        last_move_ = monotonic();
        ui.set_tooltip(nullptr);
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
            ui.set_tooltip(nullptr);
            request_paint();
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        Pt p = to_dip(lp);
        update_hover(p.x, p.y);
        Widget* w = hover_;
        ui.set_tooltip(nullptr);
        if (ui.focus() && ui.focus() != w) ui.set_focus(nullptr);
        if (w) {
            pressed_ = w;
            SetCapture(hwnd);
            if (msg == WM_LBUTTONDBLCLK) w->mouse_dbl(p.x, p.y);
            else w->mouse_down(p.x, p.y);
        }
        request_paint();
        return 0;
    }
    case WM_LBUTTONUP: {
        Pt p = to_dip(lp);
        Widget* w = pressed_;
        pressed_ = nullptr;
        ReleaseCapture();
        if (w && !ui.in_trash(w)) w->mouse_up(p.x, p.y);
        hover_ = ui.in_trash(hover_) ? nullptr : hover_;
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
            // Kolko nad suwakiem czy lista nie zmienia wartosci - przewija strone.
            Widget* w = widget_at(x, y);
            if (!(w && w->mouse_wheel(delta)) && pages_->r.contains(x, y))
                if (Widget* s = pages_->hit_test(pages_->r.right() - 2, y)) s->mouse_wheel(delta);
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
        } else if (Widget* o = ui.overlay()) {
            if (!o->key_down(static_cast<unsigned>(wp)) && wp == VK_ESCAPE) ui.close_overlay();
        } else if (ui.focus() && ui.focus()->key_down(static_cast<unsigned>(wp))) {
            // obsluzone przez pole z fokusem
        } else if (wp == VK_ESCAPE) {
            on_escape();
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
    Ui& ui = Ui::get();
    UINT w = static_cast<UINT>(ui.window.w), h = static_cast<UINT>(ui.window.h);
    ComPtr<IWICBitmap> bitmap;
    wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    if (!bitmap) return false;

    static ComPtr<ID2D1RenderTarget> shared;  // obrazki w pamieci naleza do jednego celu
    Gfx& g = Gfx::get();
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(g.factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, &rt))) return false;
    g.drop_resources();
    shared = rt;
    ui.relayout();
    ui.dt = 1.0f;  // animacje od razu w stanie koncowym
    ui.time = monotonic();
    // druga klatka: animacje dostaja koncowe wartosci ustawione w pierwszej
    render_to(rt.Get());
    if (FAILED(rt->EndDraw())) return false;
    render_to(rt.Get());
    if (FAILED(rt->EndDraw())) return false;
    ui.dirty = false;

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
    auto dirty_now = [this] {
        dirty_check_t_ = 0;
        refresh_save_button();
        return dirty_;
    };
    auto log_has = [this](const char* needle) { return diag_->log_text().find(needle) != std::string::npos; };
    auto shot = [&](const std::string& name, bool keep_toasts = false) {
        if (!keep_toasts) Ui::get().tick_toasts(monotonic() + 1000.0);
        check(render_png(options_.selftest_dir / widen(name + ".png")), "render " + name);
    };

    // --- bez plytki: to widac po pierwszym uruchomieniu ---
    for (int tab : {TAB_DASH, TAB_PEDALS, TAB_G29, TAB_PAD}) {
        go_tab(tab);
        refresh();
        shot(std::format("offline_{}", TABS[static_cast<size_t>(tab)].name));
    }

    engine_->link().simulate_connected(make_hello(), "COM9");

    // --- kazda zakladka odswieza sie z plytka ---
    for (int tab = 0; tab < TAB_COUNT; ++tab) {
        go_tab(tab);
        for (int k = 0; k < 40; ++k) {
            double phase = k * 0.3;
            int counts = static_cast<int>(std::sin(phase) * 1300);
            int axis = static_cast<int>(2048 + std::sin(phase * 0.7) * 1600);
            engine_->handle_state_for_test(make_frame(counts, (k % 8 == 0) ? 1u << (k % 32) : 1u << 3, axis));
            refresh();
            Sleep(4);
        }
        shot(std::format("tab_{}_{}", tab, TABS[static_cast<size_t>(tab)].name));
    }
    drain_log();
    check(!log_has("Błąd odświeżania") && !log_has("Błąd w oknie"), "brak bledow odswiezania w dzienniku");
    check(output_chip_->text() == "G29", "pasek: G29 (jest: " + output_chip_->text() + ")");
    check(title_.find("połączono (COM9)") != std::string::npos, "tytul okna ze stanem lacza");

    // --- panele z paska u gory ---
    go_tab(TAB_DASH);
    refresh();
    layout_root();
    open_mode_popover(output_chip_->r);
    check(Ui::get().overlay() != nullptr, "panel trybu otwarty");
    shot("popover_tryb");
    Ui::get().close_overlay();
    Ui::get().collect_garbage();
    open_link_popover();
    shot("popover_lacze");
    Ui::get().close_overlay();
    Ui::get().collect_garbage();

    // --- wskaznik niezapisanych zmian i zapis stanu okna ---
    check(!dirty_now(), "swieza konfiguracja bez niezapisanych zmian");
    go_tab(TAB_BUTTONS);
    check(!dirty_now(), "zakladka to nie ustawienie");
    cfg_.pedals[1].gamma = 1.5;
    check(dirty_now(), "zmiana ustawienia to niezapisana zmiana");
    save_config();
    {
        json::Value saved = json::parse([&] {
            std::ifstream f(config_path(), std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(f), {});
        }());
        check(std::abs(saved.find("pedals")->find("brake")->find("gamma")->to_double() - 1.5) < 1e-9, "zapis wartosci");
        check(saved.find("ui")->find("tab")->to_int(-1) == TAB_BUTTONS, "zapis zakladki");
    }
    check(!dirty_now(), "po zapisie brak niezapisanych zmian");
    shot("toast_zapis", true);

    // --- suwaki maja te same granice co normalize() ---
    go_tab(TAB_PEDALS);
    pedals_->select(0);
    refresh();
    layout_root();
    pedals_->dz_low()->apply(pedals_->dz_low()->value_at(10000));
    check(std::abs(cfg_.pedals[0].deadzone_low - 0.45) < 1e-6, "suwak martwej strefy do konca = 0.45");
    pedals_->gamma()->apply(pedals_->gamma()->value_at(10000));
    check(std::abs(cfg_.pedals[0].gamma - 3.0) < 1e-6, "suwak gamma do konca = 3.0");
    cfg_.ffb.min_duty = 200;
    cfg_.normalize();
    check(cfg_.ffb.max_duty > cfg_.ffb.min_duty, "PWM max > PWM min");
    cfg_.pedals[0].deadzone_low = 0.02;
    cfg_.pedals[0].gamma = 1.0;
    refresh();

    // --- ostrzezenia z plytki ---
    go_tab(TAB_DASH);
    engine_->handle_state_for_test(make_frame(0, 0, 1000, 0x80 | 0x20 | 0x40 | protocol::FLAG_ENCODER_ERROR | protocol::FLAG_FFB_WATCHDOG));
    refresh();
    shot("flagi_plytki");

    // --- stany G29 na pasku ---
    go_tab(TAB_DASH);
    engine_->handle_state_for_test(make_frame(0, 0, 1000, 0x40));  // odlaczona
    refresh();
    check(output_chip_->text() == "G29: podłączanie", "G29 odlaczona = podlaczanie (jest: " + output_chip_->text() + ")");
    engine_->handle_state_for_test(make_frame(0, 0, 1000, 0x80 | 0x40));  // Windows jeszcze liczy
    refresh();
    check(output_chip_->text() == "G29: wykrywanie", "G29 podlaczona bez USB = wykrywanie (jest: " + output_chip_->text() + ")");
    engine_->handle_state_for_test(make_frame());
    refresh();

    // --- przyciski: wcisniety przycisk wybiera kafelek ---
    go_tab(TAB_BUTTONS);
    engine_->handle_state_for_test(make_frame());
    refresh();
    engine_->handle_state_for_test(make_frame(0, 1u << 17));
    refresh();
    check(buttons_->selected() == 17, "wcisniety przycisk wybiera kafelek");
    engine_->handle_state_for_test(make_frame());
    refresh();

    // --- nauka przycisku pada ---
    go_tab(TAB_PAD);
    engine_->handle_state_for_test(make_frame());
    refresh();
    pad_->view()->start_learning("A");
    check(pad_->view()->canvas()->visual_state("A") == "learn_on", "nauka: kontrolka miga");
    engine_->handle_state_for_test(make_frame(0, 1u << 20));
    refresh();
    check(cfg_.pad.buttons["A"] == 20 && !pad_->view()->learning(), "nauka: przycisk 20 trafia do A");

    // przycisk nie moze sterowac dwiema kontrolkami
    engine_->handle_state_for_test(make_frame());
    refresh();
    pad_->view()->start_learning("B");
    engine_->handle_state_for_test(make_frame(0, 1u << 20));
    refresh();
    check(cfg_.pad.buttons["B"] == 20 && cfg_.pad.buttons["A"] == -1, "nauka: przycisk zabrany z poprzedniej kontrolki");

    // kreator przechodzi kolejne kroki i konczy sie na Esc
    engine_->handle_state_for_test(make_frame());
    refresh();
    pad_->wizard_start();
    check(pad_->view()->learning() == std::optional<std::string>("A"), "kreator: pierwszy krok A");
    engine_->handle_state_for_test(make_frame(0, 1u << 2));
    refresh();
    check(pad_->view()->learning() == std::optional<std::string>("B"), "kreator: po przypisaniu krok B");
    shot("pad_kreator");
    on_escape();
    check(!pad_->view()->learning() && !pad_->wizard_active(), "kreator: Esc konczy");

    // menu zrodel na osi
    pad_->on_axis_click("LT", 360, 300);
    check(Ui::get().overlay() != nullptr, "menu zrodla osi otwarte");
    shot("pad_menu");
    Ui::get().close_overlay();
    Ui::get().collect_garbage();
    pad_->set_axis_source("LT", "clutch");
    check(cfg_.pad.axes["LT"] == "clutch", "zrodlo osi zapisane");
    pad_->defaults();

    // --- G29: nauka i przepisywanie z pada ---
    go_tab(TAB_G29);
    engine_->handle_state_for_test(make_frame());
    refresh();
    g29_->view()->start_learning("GEAR_R");
    check(pages_->current() == TAB_G29, "G29: zostaje na zakladce");
    engine_->handle_state_for_test(make_frame(0, 1u << 30));
    refresh();
    check(cfg_.g29.buttons["GEAR_R"] == 30, "G29: nauka biegu wstecznego");
    g29_->copy_from_pad();
    check(cfg_.g29.buttons["GEAR_R"] == 30, "G29: przepisanie z pada zostawia biegi");
    shot("g29_po_nauce");

    // --- rysunek G29: kontrolki nie nachodza na siebie i mieszcza sie w plotnie ---
    bool in_bounds = true;
    const auto& parts = g29_->view()->canvas()->parts();
    for (const Part& part : parts) {
        float x1, y1, x2, y2;
        part.shape.bbox(x1, y1, x2, y2);
        in_bounds = in_bounds && x1 >= 0 && y1 >= 0 && x2 <= 486 && y2 <= 388;
    }
    check(in_bounds && parts.size() == G29_CONTROLS.size(), "G29: kontrolki w granicach plotna");
    bool overlap = false;
    for (float y = 0.5f; y < 388 && !overlap; y += 0.5f)
        for (float x = 0.5f; x < 486 && !overlap; x += 0.5f)
            if (g29_->view()->canvas()->parts_at(x, y).size() > 1) overlap = true;
    check(!overlap, "G29: kontrolki nie nachodza na siebie");

    bool lb = false, a_btn = false;
    for (const Part& part : pad_->view()->canvas()->parts()) {
        lb = lb || part.name == "LB";
        a_btn = a_btn || part.name == "A";
    }
    check(lb && a_btn, "pad: czesci rysunku utworzone");

    // --- male okno: waski pasek boczny, kolumny jedna pod druga ---
    Ui::get().window = {1040, 680};
    for (int tab = 0; tab < TAB_COUNT; ++tab) {
        go_tab(tab);
        engine_->handle_state_for_test(make_frame(600, 1u << 4, 2800));
        refresh();
        shot(std::format("male_{}_{}", tab, TABS[static_cast<size_t>(tab)].name));
    }
    check(sidebar_->compact(), "male okno: waski pasek boczny");

    // --- cale strony, takze to, co zwykle jest pod zwojem ---
    for (Size size : {Size{1400, 2300}, Size{1040, 3100}}) {
        Ui::get().window = size;
        for (int tab = 0; tab < TAB_COUNT; ++tab) {
            go_tab(tab);
            engine_->handle_state_for_test(make_frame(600, 1u << 4, 2800));
            refresh();
            shot(std::format("cala_{}_{}", static_cast<int>(size.w), TABS[static_cast<size_t>(tab)].name));
        }
    }
    Ui::get().window = {1400, 900};
    go_tab(TAB_DASH);
    refresh();

    // --- bez plytki podglad gasnie ---
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

    Ui::get().window = {1400, 900};
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
    report += "\n--- dziennik ---\n" + diag_->log_text() + "\n";
    report += ok ? "\nWYNIK: OK\n" : "\nWYNIK: BLEDY\n";
    std::ofstream(options_.selftest_dir / "selftest.log", std::ios::binary) << report;
    engine_->stop();
    return ok ? 0 : 1;
}

} // namespace wb::ui

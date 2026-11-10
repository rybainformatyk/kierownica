// Diagnostyka: lacze, sterowniki, zgodnosc z grami i dziennik.
#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>

#include "app.h"
#include "../util.h"
#include "../winsys.h"

namespace fs = std::filesystem;

namespace wb::ui {

namespace {

constexpr const char* VJOY_URL = "https://sourceforge.net/projects/vjoystick/";
constexpr const char* G29_BUTTON_HELP =
    "Przyciski G29 przypisujesz w zakładce G29 - kliknij przycisk na rysunku kierownicy i wciśnij go na swojej. "
    "Te przypisania nie zależą od zakładki Pad ani od listy vJoy w zakładce Przyciski.";

struct GameRow {
    const char* games;
    const char* mode;
    const char* why;
};
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

std::string fmt_duration(double seconds) {
    int s = static_cast<int>(std::max(0.0, seconds));
    int h = s / 3600, m = (s % 3600) / 60;
    s %= 60;
    return h ? std::format("{}:{:02d}:{:02d}", h, m, s) : std::format("{}:{:02d}", m, s);
}

fs::path exe_path() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    return fs::path(std::wstring(buf, n));
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

RGB level_color(const std::string& level) {
    if (level == "ok") return GOOD;
    if (level == "warn") return WARN;
    if (level == "error") return BAD;
    return TEXT_DIM;
}

int level_rank(const std::string& level) {
    if (level == "warn") return 1;
    if (level == "error") return 2;
    return 0;
}

// Cienka kreska przy wierszu tabeli zgodnosci.
class Tick final : public Widget {
public:
    Tick(RGB color, float w, float h) : color_(color), w_(w), h_(h) {}
    Size measure(float) override { return {w_, h_}; }
    Widget* hit_test(float, float) override { return nullptr; }
    void paint(Gfx& g) override { g.fill_rrect({r.x, r.y + 3, w_, r.h - 6}, 1.5f, color_); }

private:
    RGB color_;
    float w_, h_;
};

} // namespace

std::unique_ptr<Widget> DiagPage::build() {
    AppConfig& cfg = app_.cfg();
    auto page = mk<Box>(false);

    // --- kafelki lacza ---
    struct Spec { const char* key; const char* label; bool spark; };
    const Spec specs[2][4] = {{{"fps", "Ramki na sekundę", true}, {"lat", "Opóźnienie", true}, {"bps", "Przepływ", false}, {"up", "Czas pracy", false}},
                              {{"crc", "Błędy CRC", false}, {"drop", "Zgubione ramki", false}, {"resync", "Resynchronizacje", false}, {"total", "Ramek łącznie", false}}};
    for (int row = 0; row < 2; ++row) {
        auto line = mk<Columns>(std::vector<float>{1, 1, 1, 1}, 150.0f, 12.0f);
        for (const Spec& s : specs[row]) tiles_[s.key] = line->add(mk<StatTile>(s.label, s.spark));
        page->add(std::move(line), Pack{.bottom = row == 0 ? 12.0f : 16.0f});
    }

    auto cols = mk<Columns>(std::vector<float>{1, 1.2f}, 420.0f);

    // --- lewa kolumna ---
    auto left = mk<Box>(false);
    auto c1 = mk<Card>("Łącze i firmware");
    rows_["port"] = c1->body->add(mk<StatRow>("Port"));
    rows_["fw"] = c1->body->add(mk<StatRow>("Firmware"), Pack{.bottom = 4});
    bind_slider_int(c1->body, "Częstotliwość ramek płytki", "", 50, 500, &cfg.link.report_hz, Unit{0, " Hz", 1.0}, 10, LinkConfig{}.report_hz, [this] { app_.engine().apply_config(); });
    // wgrywanie firmware bez zamykania panelu - port zwalnia sam panel
    auto flash = mk<Button>(std::string("Wgraj firmware ") + protocol::FW_LATEST_TEXT, Button::Normal, [this] { app_.flash_firmware(); });
    flash->set_icon(ic::download);
    btn_flash_ = flash.get();
    btn_flash_->set_enabled(app_.firmware_available());
    add_row(c1->body, "Firmware płytki", "Zwalnia port, otwiera okno wgrywania (wgraj_firmware.bat) i łączy się ponownie, gdy je zamkniesz.", std::move(flash));
    left->add(std::move(c1), Pack{.bottom = 16});

    auto c0 = mk<Card>("Uruchamianie");
    auto autostart = mk<Toggle>(false, [this](bool v) { on_autostart(v); });
    autostart_ = autostart.get();
    add_row(c0->body, "Uruchamiaj razem z Windowsem", "Panel startuje zminimalizowany i sam łączy się z płytką. W trybie G29 gry widzą kierownicę tylko wtedy, gdy panel działa.", std::move(autostart));
    autostart_other_ = c0->body->add(mk<Label>("", Font::Small, WARN, 2000.0f), Pack{.top = 6});
    autostart_other_->visible = false;
    c0->body->add(mk<Label>("KONFIGURACJA", Font::Caps, MUTED), Pack{.top = 12, .bottom = 3});
    config_ = c0->body->add(mk<Label>(narrow(config_path().wstring()), Font::MonoSmall, TEXT_DIM, 2000.0f));
    left->add(std::move(c0));
    sync_autostart();
    cols->add(std::move(left));

    // --- prawa kolumna ---
    auto right = mk<Box>(false);
    auto g29 = mk<Card>("Kierownica G29 (Native USB)");
    const std::pair<const char*, const char*> g29_list[] = {{"fw", "Firmware z trybem G29"}, {"usb", "Windows widzi G29"}, {"feed", "Raporty z aplikacji"}, {"game", "Komendy od gry"}};
    for (const auto& [key, label] : g29_list) g29_rows_[key] = g29->body->add(mk<StatRow>(label));
    report_ = g29->body->add(mk<Label>("raport: -", Font::Mono, TEXT_DIM), Pack{.top = 6, .bottom = 4, .cross = Pack::Start});
    bind_toggle(g29->body, "Pedały jak w oryginalnym G29", "255 = puszczony. Profile gier dla G29 tego oczekują.", &cfg.g29.pedals_inverted);
    g29->body->add(note("Podłącz dwa kable: Programming Port (łącze z tą aplikacją) i Native USB (kierownica dla gier). Gdy aplikacja jest zamknięta, G29 stoi w pozycji spoczynkowej - kalibrację i mapowanie liczy tylko ona."), Pack{.top = 8});
    g29->body->add(note(G29_BUTTON_HELP), Pack{.top = 6});
    right->add(std::move(g29), Pack{.bottom = 16});

    auto vj = mk<Card>("Urządzenie wirtualne (vJoy)");
    auto device = mk<Box>(true);
    std::vector<std::string> ids;
    for (int i = 1; i <= 16; ++i) ids.push_back(std::to_string(i));
    vjoy_id_ = device->add(mk<Combo>(ids, cfg.output.vjoy_device - 1,
                                     [this](int i) {
                                         app_.cfg().output.vjoy_device = i + 1;
                                         app_.reopen_output();
                                     },
                                     5),
                           Pack{.right = 8, .cross = Pack::Center});
    device->add(mk<Button>("Podłącz ponownie", Button::Small, [this] { app_.reopen_output(true); }), Pack{.right = 6, .cross = Pack::Center});
    device->add(mk<Button>("Sprawdź vJoy", Button::Small, [this] { update_vjoy_text(); }), Pack{.cross = Pack::Center});
    add_row(vj->body, "Numer urządzenia", "", std::move(device));
    bind_toggle(vj->body, "Ukrywaj przed grami nieużywane urządzenie", "G29 albo vJoy - to, którego bieżący tryb nie używa.", &cfg.output.hide_unused, [this] { app_.reopen_output(); });
    vjoy_ = vj->body->add(mk<TextView>(9, false), Pack{.top = 8});
    auto hint = mk<Box>(true);
    auto download = mk<Button>("Pobierz vJoy", Button::Small, [] { ShellExecuteW(nullptr, L"open", widen(VJOY_URL).c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    download->set_icon(ic::download);
    hint->add(std::move(download), Pack{.cross = Pack::Center});
    hint->add(note("Potrzebne tylko w trybach z vJoy - tryb G29 działa bez niego"), Pack{.left = 10, .flex = 1, .cross = Pack::Center});
    vj->body->add(std::move(hint), Pack{.top = 10});
    right->add(std::move(vj));
    cols->add(std::move(right));
    page->add(std::move(cols), Pack{.bottom = 16});

    // --- co ktora gra potrafi zobaczyc ---
    auto compat = mk<Card>("Zgodność z grami");
    auto table = mk<Grid>(std::vector<float>{0.0f, 1.0f, -128.0f, 1.3f});
    int row = 0;
    for (const GameRow& g : GAME_COMPAT) {
        table->place(mk<Tick>(row == 0 ? ACCENT : BORDER_HI, 3.0f, 14.0f), row, 0, 1, Pack{.top = 4, .right = 10, .bottom = 4});
        table->place(mk<Label>(g.games, Font::Small, TEXT_DIM, 2000.0f), row, 1, 1, Pack{.top = 4, .right = 14, .bottom = 4});
        table->place(mk<Label>(g.mode, Font::SmallStrong, ACCENT), row, 2, 1, Pack{.top = 4, .right = 14, .bottom = 4});
        table->place(note(g.why), row, 3, 1, Pack{.top = 4, .bottom = 4});
        ++row;
    }
    compat->body->add(std::move(table));
    compat->body->add(note("Force Feedback z gry działa tylko w trybie vJoy - tamta droga idzie HID PID przez sterownik vJoy. W trybie G29 gra wysyła FFB do sterownika Logitecha, którego tu nie ma, "
                           "a w trybie pada XInput w ogóle nie ma kanału zwrotnego. W obu zostają efekty wyliczane lokalnie (sprężyna, tłumienie, tarcie) z zakładki Force Feedback."), Pack{.top = 10});
    page->add(std::move(compat), Pack{.bottom = 16});

    // --- dziennik ---
    auto c3 = mk<Card>("Dziennik");
    log_filter_ = c3->right_slot->add(mk<Segmented>(std::vector<std::string>{"Wszystko", "Ostrzeżenia", "Błędy"}, 0, [this](int i) { log_->set_filter(i); }, 30.0f), Pack{.right = 8, .cross = Pack::Center});
    auto copy = mk<Button>("Kopiuj", Button::Small, [this] { copy_log(); });
    copy->set_icon(ic::copy);
    c3->right_slot->add(std::move(copy), Pack{.right = 6, .cross = Pack::Center});
    auto clear = mk<Button>("Wyczyść", Button::Small, [this] { log_->clear(); });
    clear->set_icon(ic::trash);
    c3->right_slot->add(std::move(clear), Pack{.cross = Pack::Center});
    log_ = c3->body->add(mk<TextView>(10, true));
    page->add(std::move(c3), Pack{.bottom = 8});
    return page;
}

void DiagPage::append_log(const std::string& line, const std::string& level) { log_->append(line, level_color(level), level_rank(level)); }

std::string DiagPage::log_text() const { return log_->joined(); }

void DiagPage::set_flash_enabled(bool enabled) { btn_flash_->set_enabled(enabled); }

void DiagPage::copy_log() {
    auto hello = app_.engine().hello();
    std::string text = std::format("{} {}, tryb {}, firmware {}, C++ x64\n", APP_TITLE, APP_VERSION, app_.cfg().output.mode, hello ? hello->firmware() : "-") + log_->joined();
    // z NULL zamiast okna SetClipboardData odmawia i schowek zostaje pusty
    if (OpenClipboard(app_.window())) {
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
    Ui::get().toast("Dziennik skopiowany do schowka", GOOD, ic::copy);
}

// Zaznaczone tylko wtedy, gdy autostart wskazuje te kopie panelu. Inna kopia
// (np. wersja Python) dostaje ostrzezenie zamiast po cichu przejac wpis.
void DiagPage::sync_autostart() {
    auto command = winsys::autostart_get();
    std::optional<std::wstring> target = command ? winsys::autostart_target(*command) : std::nullopt;
    bool ours = target && lower(*target) == lower(exe_path().wstring());
    autostart_->set(ours);
    if (command && !ours) {
        autostart_other_->set("Teraz z Windowsem startuje inna kopia panelu: " + narrow(target ? *target : *command) + ". Włączenie przełączy autostart na tę kopię.");
        autostart_other_->set_visible(true);
    } else {
        autostart_other_->set_visible(false);
    }
}

void DiagPage::on_autostart(bool want) {
    if (!winsys::autostart_set(want ? std::optional<std::wstring>(winsys::autostart_command(exe_path())) : std::nullopt)) {
        app_.inform("Uruchamianie z Windowsem", "Nie udało się zmienić autostartu.", MB_ICONERROR);
    } else {
        app_.log(want ? "ok" : "info", want ? "Panel będzie startował razem z Windowsem" : "Panel nie startuje już razem z Windowsem");
    }
    sync_autostart();
}

void DiagPage::update_vjoy_text() {
    VJoyDiagnostics info = vjoy_diagnostics();
    AppConfig& cfg = app_.cfg();
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
        if (!info.enabled && cfg.output.vjoy_wanted() == false) enabled = "nie - wyłączony przez panel (tryb bez vJoy)";
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
    lines.push_back("TRYB PADA (ViGEm)");
    lines.push_back("  biblioteka : " + vg.dll_path.value_or("nie znaleziono"));
    lines.push_back(std::string("  załadowana : ") + (vg.dll_loaded ? "tak" : "NIE"));
    lines.push_back(std::string("  sterownik  : ") + (vg.driver_ok ? "działa" : "NIE DZIAŁA"));
    if (vg.error) lines.push_back("  błąd       : " + *vg.error);
    std::optional<int> slot;
    if (app_.engine().pad_open(slot))
        lines.push_back("  pad        : podłączony" + (slot ? std::format(", gniazdo XInput {}", *slot + 1) : std::string()));
    else if (cfg.output.pad_on())
        lines.push_back("  pad        : nie wystartował");
    if (!vg.driver_ok) {
        lines.push_back("");
        lines.push_back("  Zainstaluj ViGEmBus:");
        lines.push_back("  https://github.com/nefarius/ViGEmBus/releases");
    }
    vjoy_->set_lines(lines, TEXT_DIM);
    // Pole rosnie razem z trescia - czesc o padzie nie chowa sie pod spodem.
    vjoy_->set_rows(std::max(9, std::min(static_cast<int>(lines.size()) + 3, 34)));
}

void DiagPage::refresh_g29(const Frame& f) {
    const EngineStatus& st = f.st;
    AppConfig& cfg = app_.cfg();
    if (!f.connected) {
        for (auto& [key, row] : g29_rows_) row->set("-", MUTED);
        report_->set("raport: -");
        return;
    }
    g29_rows_["fw"]->set(st.g29_fw ? "tak" : std::string("nie - wgraj firmware ") + protocol::FW_LATEST_TEXT, st.g29_fw ? GOOD : BAD);
    static const std::map<std::string, std::pair<std::string, RGB>> usb_rows = {
        {"offline", {"-", MUTED}}, {"old_fw", {"-", MUTED}}, {"hidden", {"nie - ukryta, tryb bez G29", MUTED}},
        {"attaching", {"podłączanie…", WARN}}, {"detecting", {"Windows ją wykrywa…", WARN}},
        {"no_usb", {"nie - podłącz kabel do Native USB", BAD}}, {"ok", {"tak", GOOD}}};
    const auto& usb = usb_rows.at(app_.g29_state(f.connected, st));
    g29_rows_["usb"]->set(usb.first, usb.second);
    if (!cfg.output.g29_on()) g29_rows_["feed"]->set("wyłączone - inny tryb wyjścia", MUTED);
    else if (st.g29_fed) g29_rows_["feed"]->set("docierają", GOOD);
    else g29_rows_["feed"]->set("brak", WARN);
    std::string game = std::to_string(st.g29_out_packets);
    if (st.g29_game_range) game += std::format("  (zakres {} st.)", st.g29_game_range);
    g29_rows_["game"]->set(game);
    auto rep = cfg.output.g29_on() ? app_.engine().last_g29() : std::nullopt;
    report_->set(rep ? "raport: " + hex_bytes(rep->data(), rep->size()) : std::string("raport: -"));
}

void DiagPage::refresh(const Frame& f, bool visible) {
    if (!visible) return;
    sync();
    const LinkStats& stats = f.stats;
    vjoy_id_->set_index(app_.cfg().output.vjoy_device - 1);
    rows_["port"]->set(stats.port.empty() ? "-" : stats.port);
    auto hello = app_.engine().hello();
    if (!hello) rows_["fw"]->set("-", TEXT);
    else if (hello->outdated()) rows_["fw"]->set(std::format("{} {} (nowszy: {})", hello->name, hello->firmware(), protocol::FW_LATEST_TEXT), WARN);
    else rows_["fw"]->set(hello->name + " " + hello->firmware(), TEXT);

    tiles_["fps"]->set(std::format("{:.0f} Hz", stats.frames_per_sec), stats.frames_per_sec > 50 ? GOOD : WARN);
    tiles_["lat"]->set(std::format("{:.1f} ms", stats.latency_ms), stats.latency_ms < 15 ? GOOD : WARN);
    tiles_["bps"]->set(std::format("{:.1f} kB/s", stats.bytes_per_sec / 1024));
    tiles_["up"]->set(fmt_duration(stats.uptime));
    tiles_["crc"]->set(std::to_string(stats.crc_errors), stats.crc_errors == 0 ? GOOD : BAD);
    tiles_["drop"]->set(std::to_string(stats.dropped_frames), stats.dropped_frames == 0 ? GOOD : WARN);
    tiles_["resync"]->set(std::to_string(stats.resyncs));
    tiles_["total"]->set(std::to_string(stats.total_frames));
    // wykresy maja stala os czasu - jeden punkt na ~33 ms
    if (f.now - spark_t_ >= 0.033) {
        spark_t_ = f.now;
        tiles_["fps"]->push(stats.frames_per_sec, 520);
        tiles_["lat"]->push(stats.latency_ms, 40);
    }
    refresh_g29(f);
}

} // namespace wb::ui

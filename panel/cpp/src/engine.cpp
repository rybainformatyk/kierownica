#include "engine.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace wb {

namespace {

// Tresc bledu dla uzytkownika - pierwszy wiersz idzie do dziennika.
std::string first_line(const std::string& s) { return s.substr(0, s.find('\n')); }

} // namespace

WheelEngine::WheelEngine(AppConfig& cfg, LogFn on_log) : cfg_(cfg), on_log_(std::move(on_log)) {
    live_.store(std::make_shared<const AppConfig>(cfg_));
    raw_seen_.min.fill(4095);
    raw_seen_.max.fill(0);

    ffb_ = std::make_unique<FfbManager>([this](const std::string& l, const std::string& m) { log(l, m); });
    ffb_->set_direction_mode(cfg_.ffb.direction_mode);

    auto null = std::make_unique<NullBackend>();
    output_ = std::move(null);

    link_ = std::make_unique<SerialLink>(state_cb(), hello_cb(), status_cb(),
                                         [this](const std::string& l, const std::string& m) { log(l, m); },
                                         g29_out_cb());
    link_->set_on_connected([this] { push_board_config(); });
}

WheelEngine::~WheelEngine() { stop(); }

void WheelEngine::set_link_for_test(std::unique_ptr<SerialLink> link) {
    link_ = std::move(link);
    link_->set_on_connected([this] { push_board_config(); });
}

void WheelEngine::publish_config() {
    auto current = live_.load();
    if (*current == cfg_) return;
    live_.store(std::make_shared<const AppConfig>(cfg_));
    ffb_->set_direction_mode(cfg_.ffb.direction_mode);
}

void WheelEngine::start() {
    publish_config();
    link_->start();
    if (!ffb_thread_.joinable()) {
        ffb_stop_.clear();
        ffb_thread_ = std::thread([this] { ffb_loop(); });
    }
    if (cfg_.output.auto_acquire) open_output();
    if (cfg_.link.auto_connect) connect();
}

void WheelEngine::stop() {
    ffb_stop_.set();
    if (ffb_thread_.joinable()) ffb_thread_.join();
    // zerowy moment przed zamknieciem - silnik nie moze zostac pod napieciem
    link_->send(protocol::cmd_ffb(0));
    link_->send(protocol::cmd_ffb_config(false, 0, 255, false, 0, live()->ffb.timeout_ms, 65280));
    link_->stop();
    if (vjoy_thread_.joinable()) vjoy_thread_.join();
    close_output();
}

// ----------------------------------------------------------------- lacze

void WheelEngine::connect(std::optional<std::string> port) {
    std::optional<std::string> p = port;
    if (!p && !cfg_.link.port.empty()) p = cfg_.link.port;
    link_->connect(p, cfg_.link.baudrate, cfg_.link.report_hz);
}

void WheelEngine::disconnect() {
    link_->disconnect();
    {
        std::lock_guard lock(proc_mutex_);
        processor_.reset();
    }
    center_output();
}

void WheelEngine::center_wheel() {
    if (!link_->is_connected()) {
        log("warn", "Brak połączenia z płytką - środka nie ustawiono");
        return;
    }
    link_->send(protocol::cmd_center());
    // plytka wyzeruje licznik, wiec offset aplikacji tez idzie do zera
    cfg_.steer.center_counts = 0;
    publish_config();
    // Ramki w drodze maja jeszcze stary srodek - handle_state je przeczekuje.
    center_settle_until_.store(monotonic() + CENTER_SETTLE_S);
    log("ok", "Środek kierownicy ustawiony");
}

void WheelEngine::reset_board() {
    if (!link_->is_connected()) {
        log("warn", "Brak połączenia z płytką - resetu nie wysłano");
        return;
    }
    link_->send(protocol::cmd_reset());
    ffb_->reset();
    // reset ustawia tez srodek w plytce - jak center_wheel
    center_settle_until_.store(monotonic() + CENTER_SETTLE_S);
    log("info", "Wysłano reset do płytki");
}

void WheelEngine::push_board_config() {
    auto cfg = live();
    link_->send(protocol::cmd_set_rate(cfg->link.report_hz));

    long long counts = 0;
    if (cfg->steer.soft_endstop) {
        // polowa zakresu w impulsach enkodera
        counts = static_cast<long long>(cfg->steer.range_deg / 2.0 * cfg->steer.counts_per_rev / 360.0);
    }
    link_->send(protocol::cmd_set_endstop(counts, cfg->steer.endstop_gain));
    send_ffb_config(*cfg);
}

void WheelEngine::send_ffb_config(const AppConfig& cfg) {
    const FfbConfig& f = cfg.ffb;
    link_->send(protocol::cmd_ffb_config(f.enabled, f.min_duty, f.max_duty, f.invert,
                                         static_cast<long long>(f.deadband * 32767), f.timeout_ms, f.slew_per_ms));
}

void WheelEngine::set_ffb_enabled(bool enabled) {
    cfg_.ffb.enabled = enabled;
    publish_config();
    if (enabled) {
        std::lock_guard lock(out_mutex_);
        auto* backend = dynamic_cast<VJoyBackend*>(output_.get());
        if (backend && backend->is_open()) {
            if (backend->ffb_capable())
                ffb_->register_callback(backend->api(), backend->device_id());
            else
                log("warn", "Urządzenie vJoy nie ma Force Feedback - włącz 'Enable Effects' w vJoyConf");
        } else {
            log("warn", "FFB z gier wymaga działającego vJoy; efekty lokalne (sprężyna, tłumienie) działają i tak");
        }
    } else {
        ffb_->reset();
        link_->send(protocol::cmd_ffb(0));
    }
    send_ffb_config(*live());
    std::lock_guard lock(status_mutex_);
    status_.ffb_registered = ffb_->registered();
}

void WheelEngine::apply_config() {
    cfg_.normalize();
    publish_config();
    if (link_->is_connected()) push_board_config();
    // Na nowo otwieramy tylko po zmianie trybu albo numeru vJoy. Porownanie
    // z tym, co faktycznie dziala, ponawialo nieudane otwarcie przy kazdym
    // ruchu suwaka.
    std::optional<std::pair<std::string, int>> opened;
    {
        std::lock_guard lock(opened_mutex_);
        opened = opened_for_;
    }
    if (opened && *opened != std::make_pair(cfg_.output.mode, cfg_.output.vjoy_device)) open_output();
}

// --------------------------------------------------------------- wyjscie

bool WheelEngine::open_output(bool ask_again) {
    publish_config();
    std::lock_guard open_lock(open_mutex_);
    close_output();
    auto cfg = live();
    const OutputConfig& out = cfg->output;
    sync_g29_usb(true, nullptr);
    bool vjoy_later = sync_vjoy_driver(ask_again);
    bool ok = true;
    if (out.pad_on()) ok = open_pad() && ok;
    {
        std::lock_guard lock(err_mutex_);
        output_error_.clear();
    }
    if (out.wheel_on() && !vjoy_later) ok = open_wheel() && ok;
    // przy vjoy_later urzadzenie otworzy watek vJoy, gdy wlaczy sterownik
    if (out.g29_on()) {
        // G29 wystawia sama plytka - po stronie PC nie ma czego otwierac
        {
            std::lock_guard lock(g29_mutex_);
            last_g29_.reset();
        }
        log("ok", "Tryb G29: gry widzą kierownicę Logitech G29 na porcie USB płytki");
    }
    {
        std::lock_guard lock(opened_mutex_);
        opened_for_ = std::make_pair(out.mode, out.vjoy_device);
    }
    refresh_output_status();
    return ok;
}

// Wlacza vJoy w trybach z vJoy, wylacza w pozostalych - w tle. Okno UAC
// i vJoyConfig trwaja kilka sekund i zamrazalyby okno. Zwraca true, gdy tryb
// potrzebuje vJoy, a sterownik jest dopiero przelaczany.
bool WheelEngine::sync_vjoy_driver(bool ask_again) {
    auto cfg = live();
    auto want = cfg->output.vjoy_wanted();
    bool busy;
    {
        std::lock_guard lock(vjoy_mutex_);
        if (ask_again) vjoy_refused_.reset();
        busy = vjoy_running_;
    }
    if (!busy) {
        // Watek nie dziala - nowy uruchamiamy tylko wtedy, gdy stan sterownika
        // naprawde trzeba zmienic. Gdy dziala, ten odczyt bylby nieaktualny.
        if (!want) return false;
        auto current = vjoy_driver_enabled();
        if (!current || *current == *want) {
            {
                std::lock_guard lock(vjoy_mutex_);
                vjoy_refused_.reset();
            }
            std::lock_guard lock(status_mutex_);
            status_.vjoy_switch_error.clear();
            return false;
        }
        std::lock_guard lock(vjoy_mutex_);
        if (vjoy_refused_ == *want) return false;
    }
    request_vjoy_switch();
    return want.value_or(false);
}

void WheelEngine::request_vjoy_switch() {
    std::lock_guard lock(vjoy_mutex_);
    vjoy_pending_ = true;
    if (vjoy_running_) return;  // dzialajacy watek sprawdzi stan jeszcze raz
    vjoy_running_ = true;
    {
        std::lock_guard status_lock(status_mutex_);
        status_.vjoy_switching = true;
    }
    if (vjoy_thread_.joinable()) vjoy_thread_.join();  // poprzedni przebieg juz sie skonczyl
    vjoy_thread_ = std::thread([this] { vjoy_worker(); });
}

void WheelEngine::vjoy_worker() {
    for (;;) {
        {
            std::lock_guard lock(vjoy_mutex_);
            if (!vjoy_pending_) {
                vjoy_running_ = false;
                std::lock_guard status_lock(status_mutex_);
                status_.vjoy_switching = false;
                return;
            }
            vjoy_pending_ = false;
        }
        try {
            vjoy_switch_once();
        } catch (const std::exception& exc) {  // watek nie moze zginac po cichu
            log("error", std::string("Przełączanie vJoy: ") + exc.what());
        }
    }
}

void WheelEngine::vjoy_switch_once() {
    auto want = live()->output.vjoy_wanted();
    if (!want) return;
    auto current = vjoy_driver_enabled();
    bool refused;
    {
        std::lock_guard lock(vjoy_mutex_);
        refused = vjoy_refused_ == *want;
    }
    if (!current || *current == *want) {
        {
            std::lock_guard lock(vjoy_mutex_);
            vjoy_refused_.reset();
        }
        std::lock_guard lock(status_mutex_);
        status_.vjoy_switch_error.clear();
    } else if (!refused) {
        log("info", std::string(*want ? "Włączam" : "Wyłączam") + " vJoy - Windows może zapytać o zgodę administratora");
        auto [ok, msg] = set_vjoy_driver(*want, 20.0, ui_hwnd_.load());
        {
            std::lock_guard lock(vjoy_mutex_);
            if (ok) vjoy_refused_.reset();
            else vjoy_refused_ = *want;
        }
        {
            std::lock_guard lock(status_mutex_);
            status_.vjoy_switch_error = ok ? "" : msg;
        }
        if (ok) log("ok", msg);
        else log("warn", "vJoy: " + msg);
    }
    open_deferred_wheel();
}

// Otwiera vJoy, na ktore tryb czekal do wlaczenia sterownika.
void WheelEngine::open_deferred_wheel() {
    std::lock_guard open_lock(open_mutex_);
    auto cfg = live();
    const OutputConfig& out = cfg->output;
    {
        std::lock_guard lock(opened_mutex_);
        if (opened_for_ != std::make_pair(out.mode, out.vjoy_device)) return;
    }
    {
        std::lock_guard lock(out_mutex_);
        if (!out.wheel_on() || dynamic_cast<VJoyBackend*>(output_.get())) return;
    }
    {
        std::lock_guard lock(vjoy_mutex_);
        if (vjoy_pending_) return;  // zaraz kolejne przelaczenie - otworzy po nim
    }
    open_wheel();
    refresh_output_status();
}

// Podlacza G29 w trybach z G29, odlacza w pozostalych. Wolane przy zmianie
// trybu (force) i z kazda ramka STATE - wtedy tylko gdy plytka zglasza inny
// stan, np. po wlasnym resecie.
void WheelEngine::sync_g29_usb(bool force, const protocol::StateFrame* frame) {
    auto hello = link_->hello();
    if (!hello || !hello->has_g29()) return;
    bool want = live()->output.g29_wanted();
    if (!hello->can_hide_g29()) {
        if (!want && !g29_hide_warned_.exchange(true)) {
            if (hello->g29_same_cable())
                log("info", std::format("{}: G29 i łącze z panelem idą jednym gniazdem USB, więc G29 nie da się ukryć - gry widzą ją obok wybranego urządzenia.",
                                        hello->board()));
            else
                log("warn", std::format("Firmware {} nie umie ukryć G29 - gry nadal ją widzą. Wgraj firmware {}.",
                                        hello->firmware(), protocol::FW_LATEST_TEXT));
        }
        return;
    }
    double now = monotonic();
    {
        std::lock_guard lock(g29_mutex_);
        if (!force) {
            if (!frame || frame->g29_attached() == want) return;
            if (now - g29_usb_sent_t_ < G29_USB_RETRY_S) return;
        }
        g29_usb_sent_t_ = now;
    }
    link_->send(protocol::cmd_g29_usb(want));
}

// Otwieranie (pad potrafi czekac do 3 s na Windowsa) idzie bez blokady -
// pod out_mutex_ jest tylko podmiana gotowego obiektu.
bool WheelEngine::open_pad() {
    auto backend = std::make_unique<XInputBackend>();
    try {
        backend->open();
    } catch (const std::exception& exc) {
        std::string text = exc.what();
        {
            std::lock_guard lock(err_mutex_);
            pad_error_ = text;
        }
        log("error", "Pad: " + first_line(text));
        return false;
    }
    std::string status = backend->status_text();
    {
        std::lock_guard lock(out_mutex_);
        pad_output_ = std::move(backend);
    }
    {
        std::lock_guard lock(err_mutex_);
        pad_error_.clear();
    }
    log("ok", status);
    return true;
}

bool WheelEngine::open_wheel() {
    auto cfg = live();
    auto backend = std::make_unique<VJoyBackend>(cfg->output.vjoy_device);
    try {
        backend->open();
    } catch (const std::exception& exc) {
        std::string text = exc.what();
        {
            std::lock_guard lock(err_mutex_);
            output_error_ = text;
        }
        log("error", "vJoy: " + first_line(text));
        return false;
    }

    VJoyBackend* raw = backend.get();
    std::string status = backend->status_text();
    {
        std::lock_guard lock(out_mutex_);
        output_ = std::move(backend);
    }
    {
        std::lock_guard lock(err_mutex_);
        output_error_.clear();
    }
    log("ok", status);
    if (cfg->output.vjoy_buttons_only()) {
        // osie zostaja tam, gdzie zostawil je poprzedni tryb - stawiamy je
        // na srodku, zeby gra nie widziala wcisnietego gazu
        center_output();
    }
    if (cfg->ffb.enabled && raw->ffb_capable())
        ffb_->register_callback(raw->api(), raw->device_id());
    else if (cfg->ffb.enabled)
        log("warn", "Urządzenie vJoy nie ma włączonego Force Feedback - zaznacz 'Enable Effects' w vJoyConf");
    return true;
}

void WheelEngine::close_output() {
    std::lock_guard open_lock(open_mutex_);
    auto null = std::make_unique<NullBackend>();
    null->open();
    std::unique_ptr<OutputBackend> old;
    std::unique_ptr<XInputBackend> old_pad;
    {
        std::lock_guard lock(out_mutex_);
        old = std::move(output_);
        output_ = std::move(null);
        old_pad = std::move(pad_output_);
    }
    {
        std::lock_guard lock(g29_mutex_);
        last_pad_.reset();
    }
    // Watek lacza dostaje backendy tylko pod out_mutex_, wiec stare mozna
    // juz zamknac bez blokady.
    if (old) old->close();
    if (old_pad) old_pad->close();
    {
        std::lock_guard lock(opened_mutex_);
        opened_for_.reset();
    }
    refresh_output_status();
}

void WheelEngine::refresh_output_status() {
    auto cfg = live();
    std::lock_guard out_lock(out_mutex_);
    std::lock_guard lock(status_mutex_);
    status_.output_name = cfg->output.mode;
    auto* vjoy = dynamic_cast<VJoyBackend*>(output_.get());
    status_.output_ok = vjoy && vjoy->is_open();
    status_.output_text = cfg->output.g29_on() ? "Kierownica G29 na porcie USB płytki" : output_->status_text();
    status_.pad_ok = pad_output_ && pad_output_->is_open();
    status_.pad_text = pad_output_ ? pad_output_->status_text() : "Pad nieaktywny";
    status_.ffb_registered = ffb_->registered();
}

void WheelEngine::center_output() {
    auto cfg = live();
    std::lock_guard lock(out_mutex_);
    if (auto* vjoy = dynamic_cast<VJoyBackend*>(output_.get()))
        vjoy->center(neutral_axes(*cfg));
    else
        output_->center();
    if (pad_output_) pad_output_->center();
    std::lock_guard g29_lock(g29_mutex_);
    last_pad_.reset();
}

// Pozycja spoczynkowa: kierownica na srodku, pedaly puszczone.
AxisValues WheelEngine::neutral_axes(const AppConfig& cfg) const {
    AxisValues out;
    if (cfg.output.vjoy_buttons_only()) return out;  // wszystkie osie na srodku

    if (cfg.steer.vjoy_axis != "-") out.put(cfg.steer.vjoy_axis, 0.5);
    for (const PedalConfig& p : cfg.pedals)
        if (p.vjoy_axis != "-" && p.source >= 0) out.put(p.vjoy_axis, 0.0);
    return out;
}

// ---------------------------------------------------------- obsluga ramek

void WheelEngine::handle_state(const protocol::StateFrame& frame) {
    auto cfg_ptr = live();
    const AppConfig& cfg = *cfg_ptr;
    // Tuz po "Ustaw srodek" przychodza jeszcze ramki ze starym srodkiem.
    // Skok pozycji dawal zryw predkosci (tlumienie FFB szarpalo silnikiem)
    // i falszywy koniec zakresu przy wykrywaniu impulsow na obrot.
    bool settling = frame.host_time < center_settle_until_.load();
    ProcessedState state;
    {
        std::lock_guard lock(proc_mutex_);
        if (settling) processor_.steer.reset();
        state = processor_.process(frame, cfg);

        // podglad surowych zakresow dla kreatora kalibracji
        for (size_t i = 0; i < frame.axes.size(); ++i) {
            int raw = frame.axes[i];
            if (raw < raw_seen_.min[i]) raw_seen_.min[i] = raw;
            if (raw > raw_seen_.max[i]) raw_seen_.max[i] = raw;
        }
        if (settling) {
            raw_seen_.counts_min = raw_seen_.counts_max = frame.steer_counts;
        } else {
            raw_seen_.counts_min = std::min<int64_t>(raw_seen_.counts_min, frame.steer_counts);
            raw_seen_.counts_max = std::max<int64_t>(raw_seen_.counts_max, frame.steer_counts);
        }
    }
    {
        std::lock_guard lock(state_mutex_);
        last_frame_ = frame;
        last_state_ = state;
    }

    AxisValues axes;
    if (!cfg.output.vjoy_buttons_only()) {
        if (cfg.steer.vjoy_axis != "-") axes.put(cfg.steer.vjoy_axis, (state.steer_axis + 1.0) * 0.5);
        for (size_t i = 0; i < NUM_PEDALS; ++i) {
            const PedalConfig& p = cfg.pedals[i];
            if (p.vjoy_axis != "-" && p.source >= 0) axes.put(p.vjoy_axis, state.pedals[i]);
        }
    }

    {
        std::lock_guard lock(out_mutex_);
        try {
            output_->update(axes, state.out_buttons.data(), state.out_buttons_count, state.hat);
        } catch (const std::exception& exc) {
            log("error", std::string("Błąd wyjścia: ") + exc.what());
        }
        if (pad_output_) {
            try {
                PadState pad_state = build_pad_state(state, cfg);
                {
                    std::lock_guard g29_lock(g29_mutex_);
                    last_pad_ = pad_state;
                }
                pad_output_->send(pad_state);
            } catch (const std::exception& exc) {
                log("error", std::string("Błąd pada: ") + exc.what());
            }
        }
    }

    auto hello = link_->hello();
    bool attached = (hello && hello->can_hide_g29()) ? frame.g29_attached() : true;
    {
        std::unique_lock lock(status_mutex_);
        status_.g29_usb = frame.g29_usb();
        status_.g29_fed = frame.g29_fed();
        if (attached != status_.g29_attached) {
            if (attached) status_.g29_attached_since = monotonic();
            status_.g29_attached = attached;
            lock.unlock();
            log("info", attached ? "G29 podłączona do komputera" : "G29 odłączona od komputera - gry jej nie widzą");
        }
    }
    sync_g29_usb(false, &frame);
    if (cfg.output.g29_on()) send_g29(state, cfg);
}

// Raport G29 do plytki - przy zmianie i co G29_KEEPALIVE_S.
void WheelEngine::send_g29(const ProcessedState& state, const AppConfig& cfg) {
    auto hello = link_->hello();
    // Stary firmware nie zna CMD_G29 i odpowiadalby odmowa na kazda ramke.
    if (!hello || !hello->has_g29()) return;
    protocol::G29Report report = build_g29_report(state, cfg);
    double now = monotonic();
    {
        std::lock_guard lock(g29_mutex_);
        if (last_g29_ == report && now - g29_sent_t_ < G29_KEEPALIVE_S) return;
        last_g29_ = report;
        g29_sent_t_ = now;
    }
    link_->send(protocol::cmd_g29(report));
}

// Gra (przez sterownik) wyslala cos do G29 - FFB albo zakres obrotu.
void WheelEngine::handle_g29_out(const protocol::Bytes& payload) {
    uint64_t count;
    {
        std::lock_guard lock(status_mutex_);
        count = ++status_.g29_out_packets;
    }
    {
        std::lock_guard lock(g29_mutex_);
        g29_last_out_ = payload;
    }
    if (count == 1) {
        std::string hex;
        for (uint8_t b : payload) hex += std::format("{}{:02x}", hex.empty() ? "" : " ", b);
        log("info", "Gra wysyła komendy do G29: " + hex);
    }
    // f8 81 lo hi - zakres obrotu (tak ustawia go sterownik Logitecha)
    if (payload.size() >= 4 && payload[0] == 0xF8 && payload[1] == 0x81) {
        int deg = payload[2] | (payload[3] << 8);
        bool changed;
        {
            std::lock_guard lock(status_mutex_);
            changed = deg != status_.g29_game_range;
            status_.g29_game_range = deg;
        }
        if (changed) log("info", std::format("Gra ustawiła zakres obrotu G29: {} st.", deg));
    }
}

void WheelEngine::handle_hello(const protocol::HelloInfo& hello) {
    {
        std::lock_guard lock(status_mutex_);
        status_.firmware = hello.firmware();
        status_.device_name = hello.name;
        status_.board_has_ffb = hello.has_ffb();
        status_.g29_fw = hello.has_g29();
        status_.g29_unsupported = hello.g29_unsupported();
        status_.g29_same_cable = hello.g29_same_cable();
    }
    {
        std::lock_guard lock(g29_mutex_);
        last_g29_.reset();
    }
    g29_hide_warned_.store(false);
    if (live()->output.g29_on() && hello.g29_unsupported())
        log("warn", std::format("{} nie ma natywnego USB, więc nie udaje G29 - wybierz tryb vJoy albo pad Xbox 360",
                                hello.board()));
    else if (live()->output.g29_on() && !hello.has_g29())
        log("warn", std::format("Firmware {} nie ma trybu G29 - wgraj firmware {} (Diagnostyka → Wgraj firmware)",
                                hello.firmware(), protocol::FW_LATEST_TEXT));
    else if (hello.outdated())
        log("info", std::format("Płytka ma firmware {}, w panelu jest {}", hello.firmware(),
                                protocol::FW_LATEST_TEXT));
}

void WheelEngine::handle_status(LinkState state, const std::string& detail) {
    {
        std::lock_guard lock(status_mutex_);
        status_.link_state = state;
        status_.link_detail = detail;
        if (state != LinkState::Connected) {
            status_.g29_fw = false;
            status_.g29_unsupported = false;
            status_.g29_same_cable = false;
            status_.g29_usb = false;
            status_.g29_fed = false;
            status_.g29_attached = true;
        }
    }
    if (state != LinkState::Connected) {
        {
            std::lock_guard lock(proc_mutex_);
            processor_.reset();
        }
        center_output();
    }
}

// ------------------------------------------------------------- watek FFB

void WheelEngine::ffb_loop() {
    double last_cfg_push = 0.0;
    while (!ffb_stop_.is_set()) {
        auto cfg = live();
        const FfbConfig& f = cfg->ffb;
        double period = 1.0 / std::max(50, f.update_hz);
        double t0 = monotonic();

        double torque = 0.0;
        try {
            torque = compute_torque();
        } catch (const std::exception& exc) {
            log("error", std::string("Błąd obliczania FFB: ") + exc.what());
        }

        {
            std::lock_guard lock(status_mutex_);
            status_.ffb_torque = torque;
            status_.ffb_packets = ffb_->packets.load();
            status_.ffb_registered = ffb_->registered();
        }

        if (link_->is_connected() && f.enabled) {
            long long value = py_round(torque * 32767);
            value = std::max(-32767LL, std::min(32767LL, value));
            // wysylamy takze gdy sie nie zmienilo - inaczej watchdog
            // w firmware zwolnilby silnik
            link_->send(protocol::cmd_ffb(value));
            ffb_last_sent_ = value;

            // odswiezenie konfiguracji FFB co 2 s (np. po zmianie suwakow)
            if (t0 - last_cfg_push > 2.0) {
                last_cfg_push = t0;
                send_ffb_config(*cfg);
            }
        } else if (ffb_last_sent_ != 0 && link_->is_connected()) {
            link_->send(protocol::cmd_ffb(0));
            ffb_last_sent_ = 0;
        }

        double dt = monotonic() - t0;
        precise_sleep(std::max(0.0005, period - dt));
    }
}

double WheelEngine::compute_torque() {
    auto cfg = live();
    const FfbConfig& f = cfg->ffb;
    if (!f.enabled) return 0.0;
    auto state = last_state();
    if (!state) return 0.0;

    double pos = std::max(-1.0, std::min(1.0, state->steer_axis));
    // predkosc normalizujemy wzgledem 1000 st/s - tyle wyciaga reka
    double vel = std::max(-1.0, std::min(1.0, state->steer_velocity / 1000.0));

    double torque = 0.0;

    if (ffb_->registered()) {
        FfbGains gains;
        gains.constant = f.game_constant_gain;
        gains.periodic = f.game_periodic_gain;
        gains.spring = f.game_spring_gain;
        gains.damper = f.game_damper_gain;
        torque += ffb_->compute(pos, vel, gains);
    }

    // efekty lokalne - dzialaja takze gdy gra nie wysyla FFB
    if (f.spring_gain > 0.0) torque += -pos * f.spring_gain;
    if (f.damper_gain > 0.0) torque += -vel * f.damper_gain;
    if (f.friction_gain > 0.0 && std::abs(vel) > 0.005)
        torque += (vel > 0 ? -1.0 : 1.0) * f.friction_gain * std::min(1.0, std::abs(vel) * 10);

    torque *= f.overall_gain;
    torque = std::max(-1.0, std::min(1.0, torque));

    if (std::abs(torque) < f.deadband) return 0.0;
    if (f.min_force > 0.0 && torque != 0.0) {
        double sign = torque > 0 ? 1.0 : -1.0;
        torque = sign * (f.min_force + std::abs(torque) * (1.0 - f.min_force));
    }
    return std::max(-f.max_force, std::min(f.max_force, torque));
}

// ------------------------------------------------------------ kalibracja

void WheelEngine::reset_steer_processor() {
    std::lock_guard lock(proc_mutex_);
    processor_.steer.reset();
}

void WheelEngine::reset_pedal_processor(const std::string& key) {
    int idx = pedal_index(key);
    if (idx < 0) return;
    std::lock_guard lock(proc_mutex_);
    processor_.pedals[static_cast<size_t>(idx)].reset();
}

std::string WheelEngine::output_status_text() const {
    std::lock_guard lock(out_mutex_);
    return output_->status_text();
}

bool WheelEngine::pad_open(std::optional<int>& slot) const {
    std::lock_guard lock(out_mutex_);
    if (!pad_output_ || !pad_output_->is_open()) return false;
    slot = pad_output_->slot();
    return true;
}

void WheelEngine::reset_raw_tracking() {
    auto frame = last_frame();
    std::lock_guard lock(proc_mutex_);
    raw_seen_.min.fill(4095);
    raw_seen_.max.fill(0);
    int64_t base = frame ? frame->steer_counts : 0;
    raw_seen_.counts_min = base;
    raw_seen_.counts_max = base;
}

bool WheelEngine::calibrate_pedal_from_seen(const std::string& key) {
    int idx = pedal_index(key);
    if (idx < 0) return false;
    PedalConfig& p = cfg_.pedals[static_cast<size_t>(idx)];
    if (p.source < 0 || p.source >= protocol::NUM_AXES) return false;
    RawSeen seen = raw_seen();
    int lo = seen.min[static_cast<size_t>(p.source)];
    int hi = seen.max[static_cast<size_t>(p.source)];
    if (hi - lo < 64) return false;
    p.raw_min = lo;
    p.raw_max = hi;
    cfg_.normalize();
    publish_config();
    std::lock_guard lock(proc_mutex_);
    processor_.pedals[static_cast<size_t>(idx)].reset();
    return true;
}

// Wyliczenie impulsow na obrot z pelnego przekreconego zakresu.
std::optional<int> WheelEngine::detect_counts_per_rev() const {
    RawSeen seen = raw_seen();
    int64_t span = seen.counts_max - seen.counts_min;
    if (span < 100) return std::nullopt;
    // uzytkownik przekrecil lock-to-lock, czyli range_deg stopni
    long long cpr = py_round(static_cast<double>(span) * 360.0 / std::max(1, cfg_.steer.range_deg));
    if (cpr < 4) return std::nullopt;
    return static_cast<int>(std::min<long long>(cpr, 2'000'000'000));
}

// ------------------------------------------------------------- pomocnicze

EngineStatus WheelEngine::status() const {
    std::lock_guard lock(status_mutex_);
    return status_;
}

std::optional<ProcessedState> WheelEngine::last_state() const {
    std::lock_guard lock(state_mutex_);
    return last_state_;
}

std::optional<protocol::StateFrame> WheelEngine::last_frame() const {
    std::lock_guard lock(state_mutex_);
    return last_frame_;
}

std::optional<PadState> WheelEngine::last_pad() const {
    std::lock_guard lock(g29_mutex_);
    return last_pad_;
}

std::optional<protocol::G29Report> WheelEngine::last_g29() const {
    std::lock_guard lock(g29_mutex_);
    return last_g29_;
}

protocol::Bytes WheelEngine::g29_last_out() const {
    std::lock_guard lock(g29_mutex_);
    return g29_last_out_;
}

RawSeen WheelEngine::raw_seen() const {
    std::lock_guard lock(proc_mutex_);
    return raw_seen_;
}

std::string WheelEngine::output_error() const {
    std::lock_guard lock(err_mutex_);
    return output_error_;
}

std::string WheelEngine::pad_error() const {
    std::lock_guard lock(err_mutex_);
    return pad_error_;
}

std::string WheelEngine::vjoy_version_text() const {
    std::lock_guard lock(out_mutex_);
    auto* vjoy = dynamic_cast<VJoyBackend*>(output_.get());
    return vjoy ? vjoy->version_text() : std::string();
}

std::vector<std::string> WheelEngine::vjoy_available_axes() const {
    std::lock_guard lock(out_mutex_);
    auto* vjoy = dynamic_cast<VJoyBackend*>(output_.get());
    return vjoy ? vjoy->available_axes() : std::vector<std::string>();
}

int WheelEngine::vjoy_max_buttons() const {
    std::lock_guard lock(out_mutex_);
    auto* vjoy = dynamic_cast<VJoyBackend*>(output_.get());
    return vjoy ? vjoy->max_buttons() : 0;
}

bool WheelEngine::vjoy_ffb_capable() const {
    std::lock_guard lock(out_mutex_);
    auto* vjoy = dynamic_cast<VJoyBackend*>(output_.get());
    return vjoy && vjoy->ffb_capable();
}

void WheelEngine::log(const std::string& level, const std::string& msg) const {
    if (!on_log_) return;
    try {
        on_log_(level, msg);
    } catch (...) {
    }
}

} // namespace wb

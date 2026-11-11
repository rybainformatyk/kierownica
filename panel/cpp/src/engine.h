// Silnik aplikacji - spaja lacze szeregowe, przetwarzanie, wyjscie vJoy i FFB.
//
// Przeplyw danych:
//
//     plytka --(ramka STATE)--> SerialLink (watek lacza)
//                                   |
//                             StateProcessor  (kalibracja, krzywe, mapowanie)
//                                   |
//                             OutputBackend   (vJoy -> gra)
//
//     gra --(HID PID)--> vJoy --(callback)--> FfbManager
//                                                 |
//                                   watek FFB: moment -> CMD_FFB -> plytka
//
// Konfiguracje zmienia tylko watek interfejsu. Pozostale watki czytaja
// niezmienna kopie (live()), ktora interfejs publikuje przez publish_config().
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "config.h"
#include "ffb.h"
#include "link.h"
#include "outputs/base.h"
#include "outputs/vjoy.h"
#include "outputs/xinput.h"
#include "processing.h"
#include "protocol.h"
#include "util.h"

namespace wb {

struct EngineStatus {
    LinkState link_state = LinkState::Disconnected;
    std::string link_detail;
    std::string output_name = "none";
    std::string output_text = "Nie uruchomiono";
    bool output_ok = false;
    bool pad_ok = false;
    std::string pad_text = "Pad nieaktywny";
    bool ffb_registered = false;
    double ffb_torque = 0.0;
    uint64_t ffb_packets = 0;
    std::string firmware;
    std::string device_name;
    bool board_has_ffb = false;
    // Tryb G29: czy firmware to umie, czy Windows widzi G29 i czy raporty
    // z aplikacji docieraja do plytki.
    bool g29_fw = false;
    // Plytka w ogole nie umie G29 (Uno, Mega, ESP32) - nowy firmware nie pomoze.
    bool g29_unsupported = false;
    // G29 na tym samym gniezdzie USB co lacze (Leonardo, ESP32-S3); Due ma
    // osobny port Native USB.
    bool g29_same_cable = false;
    bool g29_usb = false;
    bool g29_fed = false;
    uint64_t g29_out_packets = 0;
    int g29_game_range = 0;
    // G29 podlaczona do komputera - panel odlacza ja w trybach bez G29
    bool g29_attached = true;
    // Kiedy G29 zostala ponownie podlaczona (0 = podlaczona od poczatku)
    double g29_attached_since = 0.0;
    // Dlaczego nie udalo sie wlaczyc / wylaczyc vJoy ("" = stan jak trzeba)
    std::string vjoy_switch_error;
    // Trwa wlaczanie / wylaczanie sterownika vJoy (okno UAC, vJoyConfig)
    bool vjoy_switching = false;
};

inline constexpr double G29_KEEPALIVE_S = 0.05;  // plytka wraca do spoczynku po 250 ms ciszy
inline constexpr double G29_USB_RETRY_S = 0.5;
inline constexpr double CENTER_SETTLE_S = 0.1;   // tyle po CENTER moga przychodzic stare ramki

struct RawSeen {
    std::array<int, protocol::NUM_AXES> min{};
    std::array<int, protocol::NUM_AXES> max{};
    int64_t counts_min = 0;
    int64_t counts_max = 0;
};

class WheelEngine {
public:
    WheelEngine(AppConfig& cfg, LogFn on_log);
    ~WheelEngine();

    void start();
    void stop();

    // Publikuje biezaca konfiguracje dla watkow lacza i FFB. Tylko watek UI.
    void publish_config();
    std::shared_ptr<const AppConfig> live() const { return live_.load(); }

    void connect(std::optional<std::string> port = std::nullopt);
    void disconnect();
    void center_wheel();
    void reset_board();
    void set_ffb_enabled(bool enabled);
    void apply_config();

    // Otwiera wyjscia zgodnie z trybem. False, gdy ktores zawiodlo.
    // `ask_again` ponawia prosbe o przelaczenie vJoy po wczesniejszej odmowie.
    bool open_output(bool ask_again = false);
    void close_output();

    void reset_steer_processor();
    void reset_pedal_processor(const std::string& key);
    std::string output_status_text() const;
    // Czy pad dziala i w ktorym gniezdzie XInput.
    bool pad_open(std::optional<int>& slot) const;

    void reset_raw_tracking();
    bool calibrate_pedal_from_seen(const std::string& key);
    std::optional<int> detect_counts_per_rev() const;

    EngineStatus status() const;
    LinkStats link_stats() const { return link_->stats(); }
    bool is_connected() const { return link_->is_connected(); }
    std::shared_ptr<const protocol::HelloInfo> hello() const { return link_->hello(); }

    std::optional<ProcessedState> last_state() const;
    std::optional<protocol::StateFrame> last_frame() const;
    std::optional<PadState> last_pad() const;
    std::optional<protocol::G29Report> last_g29() const;
    protocol::Bytes g29_last_out() const;
    RawSeen raw_seen() const;
    std::string output_error() const;
    std::string pad_error() const;
    std::string vjoy_version_text() const;
    std::vector<std::string> vjoy_available_axes() const;
    int vjoy_max_buttons() const;
    bool vjoy_ffb_capable() const;

    FfbManager& ffb() { return *ffb_; }
    const FfbManager& ffb() const { return *ffb_; }

    // Okno panelu - okno UAC wyskakuje nad nim.
    void set_ui_hwnd(HWND hwnd) { ui_hwnd_.store(hwnd); }

    // Dostep dla testow: podmiana lacza i przetworzenie ramki bez portu.
    SerialLink& link() { return *link_; }
    void set_link_for_test(std::unique_ptr<SerialLink> link);
    void handle_state_for_test(const protocol::StateFrame& frame) { handle_state(frame); }
    void compute_for_test(double& out) { out = compute_torque(); }

    // Fabryka lacza; testy podstawiaja wlasna.
    SerialLink::StateFn state_cb() { return [this](const protocol::StateFrame& f) { handle_state(f); }; }
    SerialLink::HelloFn hello_cb() { return [this](const protocol::HelloInfo& h) { handle_hello(h); }; }
    SerialLink::StatusFn status_cb() {
        return [this](LinkState s, const std::string& d) { handle_status(s, d); };
    }
    SerialLink::G29OutFn g29_out_cb() { return [this](const protocol::Bytes& p) { handle_g29_out(p); }; }
    void log(const std::string& level, const std::string& msg) const;

private:
    void push_board_config();
    void send_ffb_config(const AppConfig& cfg);

    bool sync_vjoy_driver(bool ask_again);
    void request_vjoy_switch();
    void vjoy_worker();
    void vjoy_switch_once();
    void open_deferred_wheel();
    void sync_g29_usb(bool force, const protocol::StateFrame* frame);

    bool open_pad();
    bool open_wheel();
    void refresh_output_status();
    void center_output();
    AxisValues neutral_axes(const AppConfig& cfg) const;

    void handle_state(const protocol::StateFrame& frame);
    void send_g29(const ProcessedState& state, const AppConfig& cfg);
    void handle_g29_out(const protocol::Bytes& payload);
    void handle_hello(const protocol::HelloInfo& hello);
    void handle_status(LinkState state, const std::string& detail);

    void ffb_loop();
    double compute_torque();

    AppConfig& cfg_;
    std::atomic<std::shared_ptr<const AppConfig>> live_;
    LogFn on_log_;

    mutable std::mutex proc_mutex_;  // procesor i podglad surowych zakresow
    StateProcessor processor_;
    RawSeen raw_seen_;

    std::unique_ptr<FfbManager> ffb_;

    // Wyjsc uzywa watek lacza (kazda ramka), a otwiera je i zamyka watek
    // interfejsu albo watek vJoy. out_mutex_ chroni podmiane backendow -
    // zamykany pad nie moze w tej samej chwili dostac raportu.
    // open_mutex_ szereguje cale otwieranie.
    mutable std::recursive_mutex out_mutex_;
    std::recursive_mutex open_mutex_;
    std::unique_ptr<OutputBackend> output_;
    std::unique_ptr<XInputBackend> pad_output_;

    mutable std::mutex opened_mutex_;
    std::optional<std::pair<std::string, int>> opened_for_;

    mutable std::mutex err_mutex_;
    std::string output_error_;
    std::string pad_error_;

    mutable std::mutex g29_mutex_;
    std::optional<protocol::G29Report> last_g29_;
    protocol::Bytes g29_last_out_;
    std::optional<PadState> last_pad_;
    double g29_sent_t_ = 0.0;
    double g29_usb_sent_t_ = 0.0;
    std::atomic<bool> g29_hide_warned_{false};

    // Przelaczanie sterownika vJoy idzie w osobnym watku. pending mowi, ze
    // tryb sie zmienil i watek ma sprawdzic stan jeszcze raz.
    mutable std::mutex vjoy_mutex_;
    bool vjoy_pending_ = false;
    bool vjoy_running_ = false;
    std::optional<bool> vjoy_refused_;
    std::thread vjoy_thread_;
    std::atomic<HWND> ui_hwnd_{nullptr};

    std::atomic<double> center_settle_until_{0.0};

    std::unique_ptr<SerialLink> link_;

    mutable std::mutex status_mutex_;
    EngineStatus status_;

    mutable std::mutex state_mutex_;
    std::optional<ProcessedState> last_state_;
    std::optional<protocol::StateFrame> last_frame_;

    std::thread ffb_thread_;
    Event ffb_stop_;
    long long ffb_last_sent_ = 0;
};

} // namespace wb

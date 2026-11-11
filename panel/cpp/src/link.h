// Warstwa transportowa: port szeregowy w osobnym watku.
//
// Odpowiada za:
//   * wyszukanie plytki na dowolnym porcie COM (wysyla IDENTIFY, czeka na HELLO),
//   * automatyczne ponowne laczenie po odlaczeniu kabla,
//   * statystyki jakosci lacza (ramki/s, bledy CRC, zgubione ramki, opoznienie),
//   * nieblokujace wysylanie komend z innych watkow.
#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "protocol.h"
#include "util.h"

namespace wb {

enum class LinkState { Disconnected, Scanning, Connected, Error };

const char* link_state_label(LinkState state);

inline constexpr double PROBE_TIMEOUT = 1.2;  // oczekiwanie na HELLO przy sprawdzaniu portu
// Klony z CH340 / CP210x / FTDI i ESP32 potrafia zresetowac sie przy otwarciu
// portu - bootloader trzyma je wtedy do ok. 2 s, zanim ruszy firmware.
inline constexpr double PROBE_TIMEOUT_BRIDGE = 2.5;
inline constexpr double STALE_TIMEOUT = 1.5;  // po tylu sekundach bez ramki lacze jest zerwane

struct LinkStats {
    double frames_per_sec = 0.0;
    double bytes_per_sec = 0.0;
    uint64_t crc_errors = 0;
    uint64_t dropped_frames = 0;
    uint64_t resyncs = 0;
    double latency_ms = 0.0;
    uint64_t total_frames = 0;
    double uptime = 0.0;
    std::string port;
    double connected_at = 0.0;
};

struct PortInfo {
    std::string device;
    std::string description;
    std::string hwid;

    std::string label() const { return description.empty() ? device : device + " - " + description; }
};

std::vector<PortInfo> list_ports();

// Heurystyka pierwszenstwa przy skanowaniu - nie wyklucza zadnego portu.
bool looks_like_arduino(const PortInfo& info);

// Port COM natywnego USB plytki (Leonardo, ESP32-S3, Native USB Due) - czesc
// urzadzenia zlozonego (&MI_ w identyfikatorze). Plytka wysyla dane dopiero,
// gdy host podniesie DTR. Mostki USB-UART (Uno, Due Programming Port, ESP32)
// DTR resetuje, wiec tam zostaje opuszczone.
bool wants_dtr(const PortInfo& info);
// Ile czekac na HELLO z tego portu.
double probe_timeout(const PortInfo& info);

// Port COM otwarty w trybie overlapped: odczyt i zapis z roznych watkow.
class SerialPort {
public:
    ~SerialPort();
    // Pusty string = sukces, inaczej opis bledu.
    std::string open(const std::string& device, int baudrate, bool dtr = false);
    void close();
    // Bajty odebrane; 0 po 50 ms ciszy; -1 po bledzie.
    int read(uint8_t* buffer, int size);
    // Czy port nadal istnieje (wykrywa wyjecie kabla).
    bool alive();
    // Calosc albo false (blad / przekroczony czas).
    bool write(const uint8_t* data, size_t size, std::string& error);

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    HANDLE read_event_ = nullptr;
    HANDLE write_event_ = nullptr;
};

// Nieblokujace lacze do plytki. Caly ruch obslugiwany w jednym watku.
class SerialLink {
public:
    using StateFn = std::function<void(const protocol::StateFrame&)>;
    using HelloFn = std::function<void(const protocol::HelloInfo&)>;
    using StatusFn = std::function<void(LinkState, const std::string&)>;
    using LogCb = std::function<void(const std::string&, const std::string&)>;
    using G29OutFn = std::function<void(const protocol::Bytes&)>;

    SerialLink(StateFn on_state, HelloFn on_hello, StatusFn on_status, LogCb on_log, G29OutFn on_g29_out);
    virtual ~SerialLink();

    void start();
    void stop();
    // Wolane w watku lacza zaraz po otrzymaniu HELLO.
    void set_on_connected(std::function<void()> hook) { on_connected_ = std::move(hook); }

    void connect(std::optional<std::string> port = std::nullopt, std::optional<int> baudrate = std::nullopt,
                 std::optional<int> report_hz = std::nullopt);
    void disconnect();
    bool is_connected() const { return state_.load() == LinkState::Connected; }
    LinkState state() const { return state_.load(); }
    std::shared_ptr<const protocol::HelloInfo> hello() const { return hello_.load(); }
    LinkStats stats() const;

    // Mozna wolac z dowolnego watku. Przy braku polaczenia dane sa pomijane.
    virtual void send(const protocol::Bytes& frame);

    // Udaje polaczona plytke - tryb --selftest, bez portu COM.
    void simulate_connected(const protocol::HelloInfo& hello, const std::string& port);

    // Rozklada wejscie jak po odebraniu z portu - uzywane przez testy.
    void dispatch_for_test(uint8_t type, const protocol::Bytes& payload, double now) {
        dispatch(type, payload, now);
    }

protected:
    void set_state(LinkState state, const std::string& detail = "");
    void set_hello(std::shared_ptr<const protocol::HelloInfo> hello) { hello_.store(std::move(hello)); }

private:
    void run();
    void do_connect();
    std::unique_ptr<SerialPort> open_port(const PortInfo& info);
    std::shared_ptr<const protocol::HelloInfo> probe(SerialPort& ser, double timeout);
    void close_port();
    void pump();
    void dispatch(uint8_t type, const protocol::Bytes& payload, double now);
    void reset_stats(const std::string& port);
    void log(const std::string& level, const std::string& msg);
    void queue_fault(const std::string& msg);

    StateFn on_state_;
    HelloFn on_hello_;
    StatusFn on_status_;
    LogCb on_log_;
    G29OutFn on_g29_out_;
    std::function<void()> on_connected_;

    std::unique_ptr<SerialPort> ser_;
    mutable std::mutex ser_lock_;
    protocol::FrameParser parser_;

    std::thread thread_;
    std::atomic<bool> stop_{false};
    Event wake_;

    mutable std::mutex req_lock_;
    std::optional<std::string> requested_port_;
    bool want_connect_ = false;
    int baudrate_ = protocol::DEFAULT_BAUD;
    int report_hz_ = 333;

    std::atomic<LinkState> state_{LinkState::Disconnected};
    std::atomic<std::shared_ptr<const protocol::HelloInfo>> hello_;
    mutable std::mutex stats_lock_;
    LinkStats stats_;

    std::atomic<bool> fault_{false};

    // liczniki okna statystyk - tylko watek lacza
    double win_t_ = 0.0;
    uint64_t win_frames_ = 0;
    uint64_t win_bytes_ = 0;
    std::optional<uint16_t> last_seq_;
    double last_frame_t_ = 0.0;
    uint32_t ping_token_ = 0;
    double ping_sent_t_ = 0.0;
    double next_ping_ = 0.0;
};

} // namespace wb

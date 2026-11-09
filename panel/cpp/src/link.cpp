#include "link.h"

#include <devguid.h>
#include <setupapi.h>
#include <initguid.h>

#include <algorithm>
#include <format>
#include <set>

// Interfejs portow szeregowych (GUID_DEVINTERFACE_COMPORT).
DEFINE_GUID(WB_GUID_COMPORT, 0x86E0D1E0, 0x8089, 0x11D0, 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73);

namespace wb {

namespace protocol_ = protocol;

const char* link_state_label(LinkState state) {
    switch (state) {
    case LinkState::Disconnected: return "Rozłączono";
    case LinkState::Scanning: return "Szukanie płytki...";
    case LinkState::Connected: return "Połączono";
    case LinkState::Error: return "Błąd";
    }
    return "";
}

namespace {

std::pair<int, long long> port_sort_key(const std::string& device) {
    std::string digits;
    for (char c : device)
        if (c >= '0' && c <= '9') digits.push_back(c);
    if (digits.empty() || digits.size() > 15) return {1, 0};
    return {0, std::stoll(digits)};
}

std::string device_property(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD prop) {
    wchar_t buf[1024];
    DWORD type = 0, size = 0;
    if (!SetupDiGetDeviceRegistryPropertyW(set, &data, prop, &type, reinterpret_cast<BYTE*>(buf), sizeof(buf), &size))
        return {};
    return narrow(std::wstring_view(buf, wcsnlen(buf, size / sizeof(wchar_t))));
}

std::string trim_copy(std::string s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

void collect_ports(const GUID* guid, DWORD flags, std::vector<PortInfo>& out, std::set<std::string>& seen) {
    HDEVINFO set = SetupDiGetClassDevsW(guid, nullptr, nullptr, flags);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA data{};
    data.cbSize = sizeof(data);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &data); ++i) {
        HKEY key = SetupDiOpenDevRegKey(set, &data, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE) continue;
        wchar_t name[256];
        DWORD size = sizeof(name), type = 0;
        LONG rc = RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<BYTE*>(name), &size);
        RegCloseKey(key);
        if (rc != ERROR_SUCCESS) continue;
        std::string device = narrow(std::wstring_view(name, wcsnlen(name, size / sizeof(wchar_t))));
        if (device.rfind("LPT", 0) == 0 || !seen.insert(device).second) continue;
        PortInfo info;
        info.device = device;
        info.description = trim_copy(device_property(set, data, SPDRP_FRIENDLYNAME));
        info.hwid = device_property(set, data, SPDRP_HARDWAREID);
        out.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(set);
}

} // namespace

std::vector<PortInfo> list_ports() {
    std::vector<PortInfo> out;
    std::set<std::string> seen;
    collect_ports(&WB_GUID_COMPORT, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE, out, seen);
    collect_ports(&GUID_DEVCLASS_PORTS, DIGCF_PRESENT, out, seen);
    std::stable_sort(out.begin(), out.end(), [](const PortInfo& a, const PortInfo& b) {
        return port_sort_key(a.device) < port_sort_key(b.device);
    });
    return out;
}

bool looks_like_arduino(const PortInfo& info) {
    std::string blob = info.description + " " + info.hwid;
    std::transform(blob.begin(), blob.end(), blob.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    for (const char* v : {"2341", "1a86", "2a03", "046d", "arduino", "due", "ch340", "usb serial", "usb-serial"})
        if (blob.find(v) != std::string::npos) return true;
    return false;
}

// ------------------------------------------------------------- SerialPort

SerialPort::~SerialPort() { close(); }

std::string SerialPort::open(const std::string& device, int baudrate) {
    std::wstring path = L"\\\\.\\" + widen(device);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) return win_error_text(GetLastError());

    SetupComm(h, 8192, 4096);

    DCB dcb{};
    dcb.DCBlength = sizeof(dcb);
    GetCommState(h, &dcb);
    dcb.BaudRate = static_cast<DWORD>(baudrate);
    dcb.ByteSize = 8;
    dcb.Parity = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE;
    // DTR/RTS w stanie niskim - inaczej otwarcie portu resetuje Due
    dcb.fDtrControl = DTR_CONTROL_DISABLE;
    dcb.fRtsControl = RTS_CONTROL_DISABLE;
    dcb.fOutX = FALSE;
    dcb.fInX = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) {
        std::string e = win_error_text(GetLastError());
        CloseHandle(h);
        return e;
    }

    // Odczyt wraca od razu z tym, co jest, a przy pustym buforze czeka do 50 ms.
    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = MAXDWORD;
    t.ReadTotalTimeoutMultiplier = MAXDWORD;
    t.ReadTotalTimeoutConstant = 50;
    t.WriteTotalTimeoutMultiplier = 0;
    t.WriteTotalTimeoutConstant = 300;
    SetCommTimeouts(h, &t);
    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR | PURGE_RXABORT | PURGE_TXABORT);

    h_ = h;
    read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return {};
}

void SerialPort::close() {
    if (h_ != INVALID_HANDLE_VALUE) {
        CancelIoEx(h_, nullptr);
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
    if (read_event_) { CloseHandle(read_event_); read_event_ = nullptr; }
    if (write_event_) { CloseHandle(write_event_); write_event_ = nullptr; }
}

int SerialPort::read(uint8_t* buffer, int size) {
    OVERLAPPED ov{};
    ov.hEvent = read_event_;
    ResetEvent(read_event_);
    DWORD got = 0;
    if (!ReadFile(h_, buffer, static_cast<DWORD>(size), &got, &ov)) {
        if (GetLastError() != ERROR_IO_PENDING) return -1;
        if (!GetOverlappedResult(h_, &ov, &got, TRUE)) return -1;
    }
    return static_cast<int>(got);
}

bool SerialPort::alive() {
    DWORD errors = 0;
    COMSTAT stat{};
    return ClearCommError(h_, &errors, &stat) != FALSE;
}

bool SerialPort::write(const uint8_t* data, size_t size, std::string& error) {
    OVERLAPPED ov{};
    ov.hEvent = write_event_;
    ResetEvent(write_event_);
    DWORD done = 0;
    if (!WriteFile(h_, data, static_cast<DWORD>(size), &done, &ov)) {
        DWORD e = GetLastError();
        if (e != ERROR_IO_PENDING) {
            error = win_error_text(e);
            return false;
        }
        if (!GetOverlappedResult(h_, &ov, &done, TRUE)) {
            error = win_error_text(GetLastError());
            return false;
        }
    }
    if (done != size) {
        error = "przekroczono czas zapisu";
        return false;
    }
    return true;
}

// ------------------------------------------------------------- SerialLink

SerialLink::SerialLink(StateFn on_state, HelloFn on_hello, StatusFn on_status, LogCb on_log, G29OutFn on_g29_out)
    : on_state_(std::move(on_state)),
      on_hello_(std::move(on_hello)),
      on_status_(std::move(on_status)),
      on_log_(std::move(on_log)),
      on_g29_out_(std::move(on_g29_out)) {}

SerialLink::~SerialLink() { stop(); }

void SerialLink::start() {
    if (thread_.joinable()) return;
    stop_.store(false);
    thread_ = std::thread([this] { run(); });
}

void SerialLink::stop() {
    stop_.store(true);
    wake_.set();
    if (thread_.joinable()) thread_.join();
    close_port();
}

void SerialLink::connect(std::optional<std::string> port, std::optional<int> baudrate, std::optional<int> report_hz) {
    {
        std::lock_guard lock(req_lock_);
        requested_port_ = (port && !port->empty()) ? port : std::nullopt;
        if (baudrate && *baudrate) baudrate_ = *baudrate;
        if (report_hz && *report_hz) report_hz_ = *report_hz;
        want_connect_ = true;
    }
    wake_.set();
}

void SerialLink::disconnect() {
    {
        std::lock_guard lock(req_lock_);
        want_connect_ = false;
    }
    wake_.set();
}

LinkStats SerialLink::stats() const {
    std::lock_guard lock(stats_lock_);
    return stats_;
}

void SerialLink::send(const protocol::Bytes& frame) {
    std::lock_guard lock(ser_lock_);
    if (!ser_) return;
    std::string error;
    if (!ser_->write(frame.data(), frame.size(), error)) queue_fault("Błąd zapisu: " + error);
}

void SerialLink::simulate_connected(const protocol::HelloInfo& hello, const std::string& port) {
    hello_.store(std::make_shared<const protocol::HelloInfo>(hello));
    reset_stats(port);
    set_state(LinkState::Connected, port);
    if (on_hello_) on_hello_(hello);
}

void SerialLink::queue_fault(const std::string& msg) {
    fault_.store(true);  // znacznik: rozlacz
    log("error", msg);
    wake_.set();
}

void SerialLink::run() {
    while (!stop_.load()) {
        try {
            bool want;
            {
                std::lock_guard lock(req_lock_);
                want = want_connect_;
            }
            bool have_port;
            {
                std::lock_guard lock(ser_lock_);
                have_port = ser_ != nullptr;
            }
            if (!want) {
                if (have_port) {
                    close_port();
                    set_state(LinkState::Disconnected);
                }
                wake_.wait(0.2);
                wake_.clear();
                continue;
            }
            if (!have_port) {
                do_connect();
                bool connected;
                {
                    std::lock_guard lock(ser_lock_);
                    connected = ser_ != nullptr;
                }
                if (!connected) {
                    // nie udalo sie - czekamy chwile i probujemy ponownie
                    wake_.wait(1.5);
                    wake_.clear();
                }
                continue;
            }
            pump();
        } catch (const std::exception& exc) {  // ostatnia linia obrony
            log("error", std::string("Wyjątek w wątku łącza: ") + exc.what());
            close_port();
            set_state(LinkState::Error, exc.what());
            Sleep(1000);
        }
    }
}

void SerialLink::do_connect() {
    std::vector<PortInfo> candidates;
    std::optional<std::string> requested;
    {
        std::lock_guard lock(req_lock_);
        requested = requested_port_;
    }
    if (requested) {
        candidates.push_back(PortInfo{*requested, "", ""});
    } else {
        auto all = list_ports();
        // najpierw to, co wyglada na Arduino
        std::stable_partition(all.begin(), all.end(), looks_like_arduino);
        candidates = std::move(all);
        if (candidates.empty()) {
            set_state(LinkState::Scanning, "Brak portów COM");
            return;
        }
    }

    set_state(LinkState::Scanning, std::format("Sprawdzanie {} portów", candidates.size()));

    for (const PortInfo& info : candidates) {
        {
            std::lock_guard lock(req_lock_);
            if (stop_.load() || !want_connect_) return;
        }
        log("info", "Sprawdzam " + info.device + "...");
        auto ser = open_port(info.device);
        if (!ser) continue;
        auto hello = probe(*ser);
        if (!hello) continue;  // destruktor zamyka port

        // sukces
        {
            std::lock_guard lock(ser_lock_);
            ser_ = std::move(ser);
        }
        fault_.store(false);
        hello_.store(hello);
        parser_.reset();
        reset_stats(info.device);
        set_state(LinkState::Connected, info.device);
        log("ok", std::format("{}: {} fw {}, {} przycisków, {} osi", info.device, hello->name, hello->firmware(),
                              hello->num_buttons, hello->num_axes));
        if (hello->proto != protocol::PROTO_VERSION)
            log("warn", std::format("Wersja protokołu płytki ({}) różni się od aplikacji ({}) - zaktualizuj firmware",
                                    hello->proto, protocol::PROTO_VERSION));
        if (on_hello_) {
            try { on_hello_(*hello); } catch (...) {}
        }
        if (on_connected_) {
            try { on_connected_(); } catch (...) {}
        }
        return;
    }

    set_state(LinkState::Scanning, "Nie znaleziono płytki");
}

std::unique_ptr<SerialPort> SerialLink::open_port(const std::string& device) {
    int baud;
    {
        std::lock_guard lock(req_lock_);
        baud = baudrate_;
    }
    auto ser = std::make_unique<SerialPort>();
    std::string error = ser->open(device, baud);
    if (!error.empty()) {
        log("warn", device + ": " + error);
        return nullptr;
    }
    return ser;
}

// Pyta plytke kim jest. Akceptuje HELLO albo (po STATE) wymusza HELLO.
std::shared_ptr<const protocol::HelloInfo> SerialLink::probe(SerialPort& ser) {
    protocol::FrameParser parser;
    double deadline = monotonic() + PROBE_TIMEOUT;
    bool asked_again = false;
    std::string error;
    auto ask = protocol::cmd_identify();
    if (!ser.write(ask.data(), ask.size(), error)) return nullptr;

    uint8_t chunk[512];
    while (monotonic() < deadline) {
        if (stop_.load()) return nullptr;
        int n = ser.read(chunk, sizeof chunk);
        if (n < 0) return nullptr;
        if (n == 0) continue;
        for (const auto& frame : parser.feed(chunk, static_cast<size_t>(n))) {
            if (frame.type == protocol::MSG_HELLO) {
                auto hello = protocol::decode_hello(frame.payload);
                if (!hello) return nullptr;
                return std::make_shared<const protocol::HelloInfo>(std::move(*hello));
            }
            if (frame.type == protocol::MSG_STATE && !asked_again) {
                // plytka gada naszym protokolem, ale HELLO wyslala przed
                // otwarciem portu - prosimy jeszcze raz
                asked_again = true;
                deadline = monotonic() + PROBE_TIMEOUT;
                if (!ser.write(ask.data(), ask.size(), error)) return nullptr;
            }
        }
    }
    return nullptr;
}

void SerialLink::close_port() {
    std::unique_ptr<SerialPort> ser;
    {
        std::lock_guard lock(ser_lock_);
        ser = std::move(ser_);
    }
    // destruktor zamyka port poza blokada
}

void SerialLink::pump() {
    SerialPort* ser;
    {
        std::lock_guard lock(ser_lock_);
        ser = ser_.get();
    }
    if (!ser) return;

    // zlecony rozlacz po bledzie zapisu
    if (fault_.exchange(false)) {
        close_port();
        set_state(LinkState::Scanning, "Utracono połączenie");
        return;
    }

    uint8_t chunk[4096];
    int n = ser->alive() ? ser->read(chunk, sizeof chunk) : -1;
    if (n < 0) {
        log("warn", "Utracono port: " + win_error_text(GetLastError()));
        close_port();
        set_state(LinkState::Scanning, "Utracono połączenie");
        return;
    }

    double now = monotonic();

    if (n > 0) {
        win_bytes_ += static_cast<uint64_t>(n);
        for (const auto& frame : parser_.feed(chunk, static_cast<size_t>(n))) dispatch(frame.type, frame.payload, now);
    }

    // ------- wykrycie zamilkniecia plytki -------
    if (last_frame_t_ != 0.0 && (now - last_frame_t_) > STALE_TIMEOUT) {
        log("warn", "Płytka przestała odpowiadać");
        close_port();
        set_state(LinkState::Scanning, "Brak danych z płytki");
        return;
    }

    // ------- okno statystyk -------
    if (now - win_t_ >= 0.5) {
        double dt = now - win_t_;
        {
            std::lock_guard lock(stats_lock_);
            stats_.frames_per_sec = static_cast<double>(win_frames_) / dt;
            stats_.bytes_per_sec = static_cast<double>(win_bytes_) / dt;
            stats_.crc_errors = parser_.crc_errors;
            stats_.resyncs = parser_.resyncs;
            if (stats_.connected_at != 0.0) stats_.uptime = now - stats_.connected_at;
        }
        win_t_ = now;
        win_frames_ = 0;
        win_bytes_ = 0;
    }

    // ------- pomiar opoznienia -------
    if (now >= next_ping_) {
        next_ping_ = now + 1.0;
        ++ping_token_;
        ping_sent_t_ = monotonic();
        send(protocol::cmd_ping(ping_token_));
    }
}

void SerialLink::dispatch(uint8_t type, const protocol::Bytes& payload, double now) {
    using namespace protocol;
    if (type == MSG_STATE) {
        auto frame = decode_state(payload, now);
        if (!frame) {
            log("warn", std::format("Uszkodzona ramka STATE: {} B, oczekiwano {} B", payload.size(), STATE_SIZE));
            return;
        }
        ++win_frames_;
        last_frame_t_ = now;
        {
            std::lock_guard lock(stats_lock_);
            ++stats_.total_frames;
            if (last_seq_) {
                unsigned gap = static_cast<unsigned>(frame->seq - *last_seq_ - 1) & 0xFFFF;
                if (gap > 0 && gap < 1000) stats_.dropped_frames += gap;
            }
        }
        last_seq_ = frame->seq;
        if (on_state_) {
            try { on_state_(*frame); } catch (...) {}
        }
    } else if (type == MSG_PONG) {
        auto pong = decode_pong(payload);
        if (!pong) return;
        if (pong->first == ping_token_ && ping_sent_t_ != 0.0) {
            std::lock_guard lock(stats_lock_);
            stats_.latency_ms = (monotonic() - ping_sent_t_) * 1000.0;
        }
    } else if (type == MSG_HELLO) {
        auto hello = decode_hello(payload);
        if (!hello) return;
        auto shared = std::make_shared<const HelloInfo>(std::move(*hello));
        hello_.store(shared);
        if (on_hello_) {
            try { on_hello_(*shared); } catch (...) {}
        }
    } else if (type == MSG_ACK) {
        if (payload.size() >= 2 && payload[1] == 0) {
            std::string name = msg_name(payload[0]);
            if (name.empty()) name = std::format("0x{:02X}", payload[0]);
            log("warn", "Płytka odrzuciła komendę " + name);
        }
    } else if (type == MSG_LOG) {
        std::string text = trim_copy(ascii_text(payload.data(), payload.size()));
        if (!text.empty()) log("info", "[płytka] " + text);
    } else if (type == MSG_G29_OUT) {
        if (on_g29_out_) {
            try { on_g29_out_(payload); } catch (...) {}
        }
    }
}

void SerialLink::reset_stats(const std::string& port) {
    double now = monotonic();
    {
        std::lock_guard lock(stats_lock_);
        stats_ = LinkStats{};
        stats_.port = port;
        stats_.connected_at = now;
    }
    win_t_ = now;
    win_frames_ = 0;
    win_bytes_ = 0;
    last_seq_.reset();
    last_frame_t_ = now;
    next_ping_ = now + 0.5;
    ping_sent_t_ = 0.0;
}

void SerialLink::set_state(LinkState state, const std::string& detail) {
    if (state == state_.load() && detail.empty()) return;
    state_.store(state);
    if (state != LinkState::Connected) hello_.store(nullptr);
    if (on_status_) {
        try { on_status_(state, detail); } catch (...) {}
    }
}

void SerialLink::log(const std::string& level, const std::string& msg) {
    if (!on_log_) return;
    try { on_log_(level, msg); } catch (...) {}
}

} // namespace wb

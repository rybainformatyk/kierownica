// Protokol KWP v1 - warstwa ramek miedzy Arduino Due a aplikacja.
//
// MUSI byc zgodny z firmware/kierownica/kierownica.ino.
//
// Ramka:
//     0xAA 0x55 | type (1B) | len (1B) | payload (len B) | CRC8 (1B)
//
// CRC8 liczony jest po [type, len, payload] - polinom 0x07, init 0x00
// (CRC-8/ATM), identycznie jak tablica CRC8_TABLE w firmware.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wb::protocol {

using Bytes = std::vector<uint8_t>;

inline constexpr int PROTO_VERSION = 1;

// Najnowszy firmware z firmware/kierownica. Starszy dziala dalej, ale bez
// nowszych funkcji - panel podpowiada wtedy, zeby go wgrac.
inline constexpr int FW_LATEST_MAJOR = 2;
inline constexpr int FW_LATEST_MINOR = 2;
inline constexpr const char* FW_LATEST_TEXT = "2.2";

inline constexpr uint8_t SYNC1 = 0xAA;
inline constexpr uint8_t SYNC2 = 0x55;
inline constexpr size_t MAX_PAYLOAD = 64;

// --- plytka -> PC ------------------------------------------------------------
inline constexpr uint8_t MSG_HELLO = 0x01;
inline constexpr uint8_t MSG_STATE = 0x02;
inline constexpr uint8_t MSG_LOG = 0x03;
inline constexpr uint8_t MSG_PONG = 0x04;
inline constexpr uint8_t MSG_ACK = 0x05;
inline constexpr uint8_t MSG_G29_OUT = 0x06;  // raport wyjsciowy, ktory gra wyslala do G29

// --- PC -> plytka ------------------------------------------------------------
inline constexpr uint8_t CMD_PING = 0x10;
inline constexpr uint8_t CMD_IDENTIFY = 0x11;
inline constexpr uint8_t CMD_CENTER = 0x12;
inline constexpr uint8_t CMD_SET_ENDSTOP = 0x13;
inline constexpr uint8_t CMD_SET_RATE = 0x14;
inline constexpr uint8_t CMD_FFB = 0x15;
inline constexpr uint8_t CMD_FFB_CONFIG = 0x16;
inline constexpr uint8_t CMD_RESET = 0x17;
inline constexpr uint8_t CMD_G29 = 0x18;      // gotowy 12-bajtowy raport G29 dla portu Native USB
inline constexpr uint8_t CMD_G29_USB = 0x19;  // 1 = podlacz G29 (Native USB) do komputera, 0 = odlacz

// Nazwa typu ramki do dziennika; pusta, gdy typ nieznany.
const char* msg_name(uint8_t type);

inline constexpr uint8_t CAP_FFB = 0x01;
inline constexpr uint8_t CAP_NATIVE_USB = 0x02;
inline constexpr uint8_t CAP_G29 = 0x04;       // firmware 2.1+: Native USB udaje kierownice G29
inline constexpr uint8_t CAP_G29_HIDE = 0x08;  // firmware 2.2+: umie odlaczyc G29 (CMD_G29_USB)

// Flagi w ramce STATE
inline constexpr uint8_t FLAG_FFB_ENABLED = 0x01;
inline constexpr uint8_t FLAG_FFB_ACTIVE = 0x02;
inline constexpr uint8_t FLAG_ENCODER_ERROR = 0x04;
inline constexpr uint8_t FLAG_ENDSTOP_HIT = 0x08;
inline constexpr uint8_t FLAG_FFB_WATCHDOG = 0x10;
inline constexpr uint8_t FLAG_G29_USB = 0x20;       // Windows wyliczyl port Native USB jako G29
inline constexpr uint8_t FLAG_G29_FED = 0x40;       // raporty G29 z aplikacji docieraja do plytki
inline constexpr uint8_t FLAG_G29_ATTACHED = 0x80;  // G29 podlaczona do komputera (firmware 2.2+)

// Dlugosc raportu wejsciowego G29 (bez numeru raportu) - patrz g29_usb.h.
inline constexpr size_t G29_REPORT_SIZE = 12;
using G29Report = std::array<uint8_t, G29_REPORT_SIZE>;

inline constexpr int NUM_AXES = 6;
inline constexpr int NUM_BUTTONS = 32;

// Domyslny baudrate. 250000 dzieli sie calkowicie przez zegary SAM3X
// (84 MHz / 16 / 21) i ATmega16U2 (16 MHz / 8 / 8) - zero bledu zegara.
inline constexpr int DEFAULT_BAUD = 250000;

inline constexpr size_t STATE_SIZE = 28;
inline constexpr size_t HELLO_SIZE = 12;

uint8_t crc8(const uint8_t* data, size_t size);

Bytes build_frame(uint8_t type, const Bytes& payload = {});

struct Frame {
    uint8_t type = 0;
    Bytes payload;
};

// Parser strumieniowy, odporny na utrate bajtow - sam sie resynchronizuje.
class FrameParser {
public:
    void reset();
    std::vector<Frame> feed(const uint8_t* data, size_t size);
    std::vector<Frame> feed(const Bytes& data) { return feed(data.data(), data.size()); }

    uint64_t crc_errors = 0;
    uint64_t resyncs = 0;
    uint64_t frames_ok = 0;

private:
    enum class St { WaitSync1, WaitSync2, Type, Len, Payload, Crc };
    St st_ = St::WaitSync1;
    uint8_t type_ = 0;
    uint8_t len_ = 0;
    Bytes buf_;
};

struct StateFrame {
    uint16_t seq = 0;
    uint32_t device_ms = 0;
    int32_t steer_counts = 0;
    std::array<uint16_t, NUM_AXES> axes{};
    uint32_t buttons = 0;
    uint8_t hat = 0;
    uint8_t flags = 0;
    double host_time = 0.0;

    bool encoder_error() const { return flags & FLAG_ENCODER_ERROR; }
    bool endstop_hit() const { return flags & FLAG_ENDSTOP_HIT; }
    bool ffb_enabled() const { return flags & FLAG_FFB_ENABLED; }
    bool ffb_watchdog() const { return flags & FLAG_FFB_WATCHDOG; }
    bool g29_usb() const { return flags & FLAG_G29_USB; }
    bool g29_fed() const { return flags & FLAG_G29_FED; }
    bool g29_attached() const { return flags & FLAG_G29_ATTACHED; }
};

struct HelloInfo {
    uint8_t proto = 0;
    uint8_t fw_major = 0;
    uint8_t fw_minor = 0;
    uint8_t caps = 0;
    uint8_t num_buttons = 0;
    uint8_t num_axes = 0;
    uint16_t report_hz = 0;
    uint32_t endstop_counts = 0;
    std::string name;

    std::string firmware() const;
    bool has_ffb() const { return caps & CAP_FFB; }
    bool native_usb() const { return caps & CAP_NATIVE_USB; }
    bool has_g29() const { return caps & CAP_G29; }
    bool can_hide_g29() const { return caps & CAP_G29_HIDE; }
    // Czy plytka ma starszy firmware niz ten z repozytorium.
    bool outdated() const {
        return std::pair<int, int>(fw_major, fw_minor) <
               std::pair<int, int>(FW_LATEST_MAJOR, FW_LATEST_MINOR);
    }
};

// Dekodery - std::nullopt, gdy ramka jest za krotka.
std::optional<StateFrame> decode_state(const Bytes& payload, double host_time = 0.0);
std::optional<HelloInfo> decode_hello(const Bytes& payload);
std::optional<std::pair<uint32_t, uint32_t>> decode_pong(const Bytes& payload);

// Tekst ASCII od plytki; bajty spoza ASCII jak errors="replace".
std::string ascii_text(const uint8_t* data, size_t size);

Bytes cmd_ping(uint32_t token);
Bytes cmd_identify();
Bytes cmd_center();
Bytes cmd_reset();
Bytes cmd_set_endstop(long long counts, long long gain = 8);
Bytes cmd_set_rate(long long hz);
Bytes cmd_g29(const G29Report& report);
Bytes cmd_g29_usb(bool attached);
Bytes cmd_ffb(long long torque);
// slew_per_ms jest wysylany jako krotnosc 256 (1 bajt).
Bytes cmd_ffb_config(bool enabled, long long min_duty, long long max_duty, bool invert,
                     long long deadband, long long timeout_ms, long long slew_per_ms);

} // namespace wb::protocol

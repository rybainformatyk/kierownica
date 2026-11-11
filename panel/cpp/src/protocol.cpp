#include "protocol.h"

#include <algorithm>
#include <cstring>

namespace wb::protocol {

const char* msg_name(uint8_t type) {
    switch (type) {
    case MSG_HELLO: return "HELLO";
    case MSG_STATE: return "STATE";
    case MSG_LOG: return "LOG";
    case MSG_PONG: return "PONG";
    case MSG_ACK: return "ACK";
    case MSG_G29_OUT: return "G29_OUT";
    case CMD_PING: return "PING";
    case CMD_IDENTIFY: return "IDENTIFY";
    case CMD_CENTER: return "CENTER";
    case CMD_SET_ENDSTOP: return "SET_ENDSTOP";
    case CMD_SET_RATE: return "SET_RATE";
    case CMD_FFB: return "FFB";
    case CMD_FFB_CONFIG: return "FFB_CONFIG";
    case CMD_RESET: return "RESET";
    case CMD_G29: return "G29";
    case CMD_G29_USB: return "G29_USB";
    default: return "";
    }
}

namespace {

constexpr std::array<uint8_t, 256> build_crc8_table() {
    std::array<uint8_t, 256> table{};
    for (int i = 0; i < 256; ++i) {
        unsigned c = static_cast<unsigned>(i);
        for (int b = 0; b < 8; ++b) c = (c & 0x80) ? ((c << 1) ^ 0x07) & 0xFF : (c << 1) & 0xFF;
        table[static_cast<size_t>(i)] = static_cast<uint8_t>(c);
    }
    return table;
}

constexpr auto CRC8_TABLE = build_crc8_table();

template <typename T>
T read_le(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));  // x64 jest little-endian, jak plytka
    return v;
}

template <typename T>
void put_le(Bytes& out, T v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    out.insert(out.end(), p, p + sizeof(T));
}

long long clamp_ll(long long v, long long lo, long long hi) { return std::max(lo, std::min(hi, v)); }

// Dzielenie jak // w Pythonie - w dol, takze dla liczb ujemnych.
long long floor_div(long long a, long long b) {
    long long q = a / b;
    return ((a % b != 0) && ((a < 0) != (b < 0))) ? q - 1 : q;
}

} // namespace

uint8_t crc8(const uint8_t* data, size_t size) {
    uint8_t c = 0;
    for (size_t i = 0; i < size; ++i) c = CRC8_TABLE[c ^ data[i]];
    return c;
}

Bytes build_frame(uint8_t type, const Bytes& payload) {
    Bytes body;
    body.reserve(payload.size() + 2);
    body.push_back(type);
    body.push_back(static_cast<uint8_t>(payload.size()));
    body.insert(body.end(), payload.begin(), payload.end());

    Bytes out;
    out.reserve(body.size() + 3);
    out.push_back(SYNC1);
    out.push_back(SYNC2);
    out.insert(out.end(), body.begin(), body.end());
    out.push_back(crc8(body.data(), body.size()));
    return out;
}

void FrameParser::reset() {
    st_ = St::WaitSync1;
    buf_.clear();
}

std::vector<Frame> FrameParser::feed(const uint8_t* data, size_t size) {
    std::vector<Frame> out;
    for (size_t i = 0; i < size; ++i) {
        uint8_t b = data[i];
        switch (st_) {
        case St::WaitSync1:
            if (b == SYNC1) st_ = St::WaitSync2;
            break;
        case St::WaitSync2:
            if (b == SYNC2) {
                st_ = St::Type;
            } else if (b != SYNC1) {  // przy SYNC1 zostajemy - 0xAA 0xAA 0x55 tez jest poprawne
                st_ = St::WaitSync1;
                ++resyncs;
            }
            break;
        case St::Type:
            type_ = b;
            st_ = St::Len;
            break;
        case St::Len:
            if (b > MAX_PAYLOAD) {
                st_ = St::WaitSync1;
                ++resyncs;
                break;
            }
            len_ = b;
            buf_.clear();
            st_ = b == 0 ? St::Crc : St::Payload;
            break;
        case St::Payload:
            buf_.push_back(b);
            if (buf_.size() >= len_) st_ = St::Crc;
            break;
        case St::Crc: {
            Bytes body;
            body.reserve(buf_.size() + 2);
            body.push_back(type_);
            body.push_back(len_);
            body.insert(body.end(), buf_.begin(), buf_.end());
            if (crc8(body.data(), body.size()) == b) {
                out.push_back(Frame{type_, buf_});
                ++frames_ok;
            } else {
                ++crc_errors;
            }
            st_ = St::WaitSync1;
            break;
        }
        }
    }
    return out;
}

std::string HelloInfo::firmware() const {
    return std::to_string(fw_major) + "." + std::to_string(fw_minor);
}

const BoardInfo* board_by_key(const std::string& key) {
    for (const BoardInfo& b : BOARDS)
        if (key == b.key) return &b;
    return nullptr;
}

const BoardInfo* board_by_hello(const std::string& hello_name) {
    for (const BoardInfo& b : BOARDS)
        if (hello_name == b.hello) return &b;
    return nullptr;
}

std::string board_label(const std::string& hello_name) {
    // Uno i Nano oraz Leonardo i Micro maja ten sam firmware - z HELLO nie
    // da sie ich rozroznic.
    if (hello_name == "KIEROWNICA-UNO") return "Arduino Uno / Nano";
    if (hello_name == "KIEROWNICA-LEONARDO") return "Arduino Leonardo / Micro";
    if (const BoardInfo* b = board_by_hello(hello_name)) return b->label;
    return hello_name;
}

std::optional<StateFrame> decode_state(const Bytes& payload, double host_time) {
    if (payload.size() < STATE_SIZE) return std::nullopt;
    const uint8_t* p = payload.data();
    StateFrame f;
    f.seq = read_le<uint16_t>(p);
    f.device_ms = read_le<uint32_t>(p + 2);
    f.steer_counts = read_le<int32_t>(p + 6);
    for (int i = 0; i < NUM_AXES; ++i) f.axes[static_cast<size_t>(i)] = read_le<uint16_t>(p + 10 + 2 * i);
    f.buttons = read_le<uint32_t>(p + 22);
    f.hat = p[26];
    f.flags = p[27];
    f.host_time = host_time;
    return f;
}

std::string ascii_text(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) {
        if (data[i] < 0x80) out.push_back(static_cast<char>(data[i]));
        else out += "\xEF\xBF\xBD";  // U+FFFD
    }
    return out;
}

std::optional<HelloInfo> decode_hello(const Bytes& payload) {
    if (payload.size() < HELLO_SIZE) return std::nullopt;
    const uint8_t* p = payload.data();
    HelloInfo h;
    h.proto = p[0];
    h.fw_major = p[1];
    h.fw_minor = p[2];
    h.caps = p[3];
    h.num_buttons = p[4];
    h.num_axes = p[5];
    h.report_hz = read_le<uint16_t>(p + 6);
    h.endstop_counts = read_le<uint32_t>(p + 8);
    h.name = ascii_text(p + HELLO_SIZE, payload.size() - HELLO_SIZE);
    return h;
}

std::optional<std::pair<uint32_t, uint32_t>> decode_pong(const Bytes& payload) {
    if (payload.size() < 8) return std::nullopt;
    return std::make_pair(read_le<uint32_t>(payload.data()), read_le<uint32_t>(payload.data() + 4));
}

Bytes cmd_ping(uint32_t token) {
    Bytes p;
    put_le(p, token);
    return build_frame(CMD_PING, p);
}

Bytes cmd_identify() { return build_frame(CMD_IDENTIFY); }
Bytes cmd_center() { return build_frame(CMD_CENTER); }
Bytes cmd_reset() { return build_frame(CMD_RESET); }

Bytes cmd_set_endstop(long long counts, long long gain) {
    Bytes p;
    put_le(p, static_cast<int32_t>(clamp_ll(counts, 0, 2'000'000'000)));
    put_le(p, static_cast<uint16_t>(clamp_ll(gain, 1, 65535)));
    return build_frame(CMD_SET_ENDSTOP, p);
}

Bytes cmd_set_rate(long long hz) {
    Bytes p;
    put_le(p, static_cast<uint16_t>(clamp_ll(hz, 50, 500)));
    return build_frame(CMD_SET_RATE, p);
}

Bytes cmd_g29(const G29Report& report) {
    return build_frame(CMD_G29, Bytes(report.begin(), report.end()));
}

Bytes cmd_g29_usb(bool attached) { return build_frame(CMD_G29_USB, Bytes{uint8_t(attached ? 1 : 0)}); }

Bytes cmd_ffb(long long torque) {
    Bytes p;
    put_le(p, static_cast<int16_t>(clamp_ll(torque, -32767, 32767)));
    return build_frame(CMD_FFB, p);
}

Bytes cmd_ffb_config(bool enabled, long long min_duty, long long max_duty, bool invert,
                     long long deadband, long long timeout_ms, long long slew_per_ms) {
    Bytes p;
    p.push_back(enabled ? 1 : 0);
    p.push_back(static_cast<uint8_t>(clamp_ll(min_duty, 0, 255)));
    p.push_back(static_cast<uint8_t>(clamp_ll(max_duty, 0, 255)));
    p.push_back(invert ? 1 : 0);
    put_le(p, static_cast<uint16_t>(clamp_ll(deadband, 0, 65535)));
    put_le(p, static_cast<uint16_t>(clamp_ll(timeout_ms, 20, 65535)));
    p.push_back(static_cast<uint8_t>(clamp_ll(floor_div(slew_per_ms, 256), 0, 255)));
    return build_frame(CMD_FFB_CONFIG, p);
}

} // namespace wb::protocol

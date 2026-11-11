// Testy czesci bez interfejsu - bez plytki i bez sterownikow.
//
//   core_tests                 same testy
//   core_tests golden.json     dodatkowo porownanie z wynikami wersji Python
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "config.h"
#include "engine.h"
#include "ffb.h"
#include "json.h"
#include "link.h"
#include "processing.h"
#include "protocol.h"
#include "updater.h"
#include "version.h"
#include "winsys.h"

using namespace wb;
using protocol::Bytes;
namespace fs = std::filesystem;

static int g_failed = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failed;                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

static bool near_eq(double a, double b, double eps = 1e-9) { return std::abs(a - b) <= eps; }

// ------------------------------------------------------------- protokol

static void test_protocol() {
    Bytes abc = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    CHECK(protocol::crc8(abc.data(), abc.size()) == 0xF4);  // CRC-8/ATM: wektor kontrolny

    Bytes frame = protocol::build_frame(0x42, {1, 2, 3});
    CHECK(frame.size() == 8);
    CHECK(frame[0] == 0xAA && frame[1] == 0x55 && frame[2] == 0x42 && frame[3] == 3);

    protocol::FrameParser p;
    auto out = p.feed(frame);
    CHECK(out.size() == 1 && out[0].type == 0x42 && out[0].payload == Bytes({1, 2, 3}));

    // resynchronizacja po smieciach i podwojnym SYNC1
    Bytes noisy = {0x00, 0xAA, 0x01, 0xAA, 0xAA};
    noisy.insert(noisy.end(), frame.begin() + 1, frame.end());
    protocol::FrameParser p2;
    CHECK(p2.feed(noisy).size() == 1);
    CHECK(p2.resyncs == 1);

    // zly CRC
    Bytes bad = frame;
    bad.back() ^= 0xFF;
    protocol::FrameParser p3;
    CHECK(p3.feed(bad).empty());
    CHECK(p3.crc_errors == 1);

    // ramka kawalkami
    protocol::FrameParser p4;
    size_t got = 0;
    for (uint8_t b : frame) got += p4.feed(&b, 1).size();
    CHECK(got == 1);

    Bytes state(28, 0);
    state[0] = 0x34; state[1] = 0x12;
    state[6] = 0xFF; state[7] = 0xFF; state[8] = 0xFF; state[9] = 0xFF;  // -1
    state[10] = 0xFF; state[11] = 0x0F;                                   // A0 = 4095
    state[22] = 0x01; state[27] = protocol::FLAG_G29_ATTACHED;
    auto s = protocol::decode_state(state, 1.5);
    CHECK(s && s->seq == 0x1234 && s->steer_counts == -1 && s->axes[0] == 4095 && s->buttons == 1);
    CHECK(s && s->g29_attached() && !s->g29_usb() && s->host_time == 1.5);
    CHECK(!protocol::decode_state(Bytes(27, 0)));

    Bytes hello = {1, 2, 1, protocol::CAP_G29, 32, 6, 0x4D, 0x01, 0, 0, 0, 0, 'D', 'u', 'e', 0xFF};
    auto h = protocol::decode_hello(hello);
    CHECK(h && h->firmware() == "2.1" && h->report_hz == 333 && h->has_g29() && !h->can_hide_g29());
    CHECK(h && h->outdated());
    CHECK(h && h->name == "Due\xEF\xBF\xBD");

    CHECK(protocol::cmd_set_rate(9999) == protocol::cmd_set_rate(500));
    CHECK(protocol::cmd_ffb(99999) == protocol::cmd_ffb(32767));
}

// ---------------------------------------------------------------- JSON

static void test_json() {
    using json::Value;
    auto v = json::parse(R"({"a": [1, 2.5, "x\n\u0105", true, null], "a2": {}, "b": 1e3, "dup": 1, "dup": 2})");
    CHECK(v.is_object());
    CHECK(v.find("a")->items().size() == 5);
    CHECK(v.find("a")->items()[2].as_string() == "x\nć" || v.find("a")->items()[2].as_string() == "x\n\xC4\x85");
    CHECK(v.find("dup")->as_int() == 2);
    CHECK(v.find("b")->as_double() == 1000.0);

    bool threw = false;
    for (const char* bad : {"{", "[1,]", "{\"a\":1,}", "01", "tru", "\"\n\""}) {
        threw = false;
        try { json::parse(bad); } catch (const json::ParseError&) { threw = true; }
        CHECK(threw);
    }

    CHECK(Value("12").to_int(7) == 12);
    CHECK(Value(" 1_000 ").to_int(7) == 1000);
    CHECK(Value("12.5").to_int(7) == 7);
    CHECK(Value(12.9).to_int(7) == 12);
    CHECK(Value(-12.9).to_int(7) == -12);
    CHECK(Value(true).to_int(7) == 1);
    CHECK(Value().to_int(7) == 7);
    CHECK(std::isnan(Value("abc").to_double()));
    CHECK(Value("1.5").to_double() == 1.5);
    CHECK(json::is_valid_utf8("zażółć"));
    CHECK(!json::is_valid_utf8("\xC4"));
    CHECK(!json::is_valid_utf8("\xFF\xFE"));
}

// ------------------------------------------------------------- konfiguracja

struct TempDir {
    fs::path path;
    TempDir() {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        path = fs::path(base) / (L"wb_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                 std::to_wstring(GetTickCount64()));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

static void write_text(const fs::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    f << text;
}

static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void test_config() {
    TempDir dir;
    fs::path path = dir.path / "config.json";

    AppConfig missing = AppConfig::load(path);
    CHECK(last_load_problem.empty());
    CHECK(missing == AppConfig());

    AppConfig cfg;
    cfg.steer.range_deg = 540;
    cfg.pedals[1].deadzone_low = 0.1;
    cfg.pad.axes["RX+"] = "clutch";
    cfg.g29.buttons["PS"] = 20;
    cfg.ui.geometry = "1000x700+1+2";
    cfg.save(path);
    AppConfig back = AppConfig::load(path);
    CHECK(last_load_problem.empty());
    CHECK(back == cfg);

    // uszkodzony plik trafia do .bad, a nie przepada
    write_text(path, "{ to nie json");
    AppConfig bad = AppConfig::load(path);
    CHECK(!last_load_problem.empty());
    CHECK(bad == AppConfig());
    CHECK(read_text(dir.path / "config.json.bad") == "{ to nie json");

    // zepsuta sekcja dostaje domyslne tylko u siebie
    write_text(path, R"({"steer": null, "ffb": {"overall_gain": "abc", "slew_per_ms": "x"},
                         "link": {"report_hz": 100000}, "pedals": {"brake": 5},
                         "buttons": {"mapping": [3, "x", 999], "hat": "nope", "invert": [1, 0]}})");
    AppConfig partial = AppConfig::load(path);
    CHECK(last_load_problem.empty());
    CHECK(partial.steer == SteerConfig());
    CHECK(partial.ffb.overall_gain == 0.0);    // float("abc") -> dolna granica
    CHECK(partial.ffb.slew_per_ms == 4000);    // int("x") -> domyslna
    CHECK(partial.link.report_hz == 500);
    CHECK(partial.pedals[1] == [] { PedalConfig p; p.source = 0; return p; }());
    CHECK(partial.buttons.mapping[0] == 3 && partial.buttons.mapping[1] == 2 && partial.buttons.mapping[2] == 128);
    CHECK(partial.buttons.mapping[31] == 32);
    CHECK(partial.buttons.hat == std::vector<int>({-1, -1, -1, -1}));
    CHECK(partial.buttons.invert[0] == true && partial.buttons.invert[1] == false);

    // odwrocony zakres pedalu
    write_text(path, R"({"pedals": {"throttle": {"source": 0, "raw_min": 3000, "raw_max": 100}}})");
    AppConfig inverted = AppConfig::load(path);
    CHECK(inverted.pedals[0].raw_min == 100 && inverted.pedals[0].raw_max == 3000 && inverted.pedals[0].invert);

    // migracje
    write_text(path, R"({"output": {"backend": "vjoy"}})");
    CHECK(AppConfig::load(path).output.mode == "wheel");
    write_text(path, R"({"output": {"backend": "none"}})");
    CHECK(AppConfig::load(path).output.mode == "none");
    write_text(path, R"({"pad": {"steer_target": "RX", "throttle_target": "LY", "brake_target": "LT"}})");
    AppConfig legacy = AppConfig::load(path);
    CHECK(legacy.pad.axes["RX-"] == "steer_left" && legacy.pad.axes["RX+"] == "steer_right");
    CHECK(legacy.pad.axes["LY+"] == "throttle" && legacy.pad.axes["LT"] == "brake" && legacy.pad.axes["LX-"].empty());
    write_text(path, R"({"pad": {"buttons": {"A": 17, "GUIDE": 3}}})");
    AppConfig from_pad = AppConfig::load(path);
    CHECK(from_pad.g29.buttons["CROSS"] == 17 && from_pad.g29.buttons["PS"] == 3);

    // profile pada
    PadConfig pad;
    CHECK(pad.apply_profile("forza") && pad.steer_range_deg == 450 && pad.steer_antideadzone == 0.15);
    CHECK(pad.detect_profile() == "forza");
    CHECK(!pad.apply_profile("nie ma") && pad.profile.empty());

    OutputConfig out;
    out.mode = "pad";
    CHECK(out.vjoy_wanted() == false && !out.g29_wanted());
    out.mode = "none";
    CHECK(!out.vjoy_wanted().has_value());
    out.mode = "g29_vjoy";
    CHECK(out.vjoy_wanted() == true && out.g29_wanted() && out.vjoy_buttons_only());
}

// ---------------------------------------------------------- przetwarzanie

static protocol::StateFrame frame_with(int32_t counts, uint32_t buttons = 0) {
    protocol::StateFrame f;
    f.steer_counts = counts;
    f.buttons = buttons;
    return f;
}

static void test_processing() {
    PedalConfig p;
    p.deadzone_low = 0.0;
    p.deadzone_high = 0.0;
    CHECK(near_eq(pedal_position(4095, p), 1.0));
    CHECK(near_eq(pedal_curve(0.5, p), 0.5));
    p.gamma = 2.0;
    CHECK(near_eq(pedal_curve(0.5, p), 0.25));

    CHECK(near_eq(steer_curve(0.25, 2.0), 0.0625));
    CHECK(near_eq(steer_curve(-0.25, 2.0), -0.0625));
    CHECK(near_eq(steer_curve(5.0, 1.0), 1.0));

    AppConfig cfg;
    cfg.steer.counts_per_rev = 3600;
    cfg.steer.range_deg = 360;
    StateProcessor proc;
    auto s = proc.process(frame_with(900), cfg, 1.0);
    CHECK(near_eq(s.steer_deg, 90.0));
    CHECK(near_eq(s.steer_axis, 0.5));

    // inwersja NC i krzyzak zabieraja przycisk z listy
    cfg.buttons.invert[0] = true;
    cfg.buttons.hat = {1, -1, -1, -1};
    auto b = proc.process(frame_with(0, 0b010), cfg, 2.0);
    CHECK(b.buttons_effective == 0b011);
    CHECK(b.hat == 0);
    CHECK(b.out_buttons_count == 1 && b.out_buttons[0] == 1);

    // raport G29: srodek, pedaly puszczone (odwrocone), krzyzak w spoczynku
    AppConfig g;
    ProcessedState center;
    auto r = build_g29_report(center, g);
    CHECK(r[0] == 8 && r[1] == 0 && r[2] == 0 && r[3] == 0);
    CHECK((r[4] | (r[5] << 8)) == 32768);  // round(32767.5) do parzystej
    CHECK(r[6] == 255 && r[7] == 255 && r[8] == 255);
    CHECK(r[9] == 0x80 && r[10] == 0x80 && r[11] == 0);

    ProcessedState pressed;
    pressed.buttons_effective = 1;  // przycisk 0 -> A -> CROSS
    pressed.steer_axis = 1.0;
    auto r2 = build_g29_report(pressed, g);
    CHECK((r2[0] & 0xF0) == 0x10);
    CHECK((r2[4] | (r2[5] << 8)) == 65535);
    CHECK(g29_active(r2)["CROSS"]);
    CHECK(!g29_active(r2)["DPAD_UP"]);

    // pad: skret na lewym drazku, gaz na RT
    AppConfig pc;
    pc.pad.steer_range_deg = 360;
    ProcessedState ps;
    ps.steer_deg = 180.0;
    ps.pedals[0] = 1.0;
    auto pad = build_pad_state(ps, pc);
    CHECK(pad.lx == 32767 && pad.rt == 255 && pad.lt == 0 && pad.ly == 0);
    ps.steer_deg = -180.0;
    CHECK(build_pad_state(ps, pc).lx == -32768);

    // anty-martwa strefa: zero zostaje zerem
    pc.pad.steer_antideadzone = 0.2;
    ps.steer_deg = 0.0;
    CHECK(build_pad_state(ps, pc).lx == 0);
    ps.steer_deg = 1.0;
    CHECK(build_pad_state(ps, pc).lx > 6000);
}

// ------------------------------------------------------------------- FFB

static void test_ffb() {
    FfbManager m;
    Effect e;
    e.index = 1;
    e.type = ET_CONST;
    e.magnitude = 0.5;
    e.active = true;
    e.start_time = 1.0;
    m.store_effect_for_test(e);
    CHECK(near_eq(m.compute(0.0, 0.0, {}, 1.1), 0.5));

    FfbGains g;
    g.constant = 0.5;
    CHECK(near_eq(m.compute(0.0, 0.0, g, 1.1), 0.25));

    // sprezyna ciagnie do srodka z nasyceniem
    Effect sp;
    sp.index = 2;
    sp.type = ET_SPRNG;
    sp.pos_coeff = 1.0;
    sp.neg_coeff = 1.0;
    sp.pos_sat = 0.3;
    sp.neg_sat = 0.3;
    sp.active = true;
    sp.start_time = 1.0;
    FfbManager m2;
    m2.store_effect_for_test(sp);
    CHECK(near_eq(m2.compute(0.2, 0.0, {}, 1.1), -0.2));
    CHECK(near_eq(m2.compute(0.9, 0.0, {}, 1.1), -0.3));
    CHECK(near_eq(m2.compute(-0.9, 0.0, {}, 1.1), 0.3));

    // efekt z czasem trwania wygasa
    Effect t;
    t.index = 3;
    t.type = ET_CONST;
    t.magnitude = 1.0;
    t.duration_ms = 100;
    t.active = true;
    t.start_time = 1.0;
    FfbManager m3;
    m3.store_effect_for_test(t);
    CHECK(near_eq(m3.compute(0, 0, {}, 1.05), 1.0));
    CHECK(near_eq(m3.compute(0, 0, {}, 1.5), 0.0));
    CHECK(m3.active_effects().empty());
}

// ---------------------------------------------------------------- silnik

class FakeLink final : public SerialLink {
public:
    using SerialLink::SerialLink;
    std::vector<Bytes> sent;
    std::mutex m;
    void send(const Bytes& frame) override {
        std::lock_guard lock(m);
        sent.push_back(frame);
    }
    void connected(const protocol::HelloInfo& hello) {
        set_hello(std::make_shared<const protocol::HelloInfo>(hello));
        set_state(LinkState::Connected, "COM1");
    }
    size_t count(uint8_t type) {
        std::lock_guard lock(m);
        size_t n = 0;
        for (const auto& f : sent)
            if (f[2] == type) ++n;
        return n;
    }
};

static void test_engine() {
    AppConfig cfg;
    cfg.output.mode = "none";
    cfg.output.auto_acquire = false;
    cfg.link.auto_connect = false;
    std::vector<std::string> logs;
    std::mutex log_mutex;
    WheelEngine eng(cfg, [&](const std::string&, const std::string& m) {
        std::lock_guard lock(log_mutex);
        logs.push_back(m);
    });
    auto fake = std::make_unique<FakeLink>(eng.state_cb(), eng.hello_cb(), eng.status_cb(), nullptr, eng.g29_out_cb());
    FakeLink* link = fake.get();
    eng.set_link_for_test(std::move(fake));

    protocol::HelloInfo hello;
    hello.proto = 1;
    hello.fw_major = 2;
    hello.fw_minor = 2;
    hello.caps = protocol::CAP_G29 | protocol::CAP_G29_HIDE | protocol::CAP_FFB;
    link->connected(hello);
    CHECK(eng.status().link_state == LinkState::Connected);

    // tryb G29: raport przy zmianie, bez zalewania przy tym samym stanie
    cfg.output.mode = "g29";
    eng.publish_config();
    auto f = frame_with(0);
    f.flags = protocol::FLAG_G29_ATTACHED;
    eng.handle_state_for_test(f);
    CHECK(link->count(protocol::CMD_G29) == 1);
    eng.handle_state_for_test(f);
    CHECK(link->count(protocol::CMD_G29) == 1);
    auto last = eng.last_g29();
    CHECK(last.has_value());
    f.steer_counts = 500;
    eng.handle_state_for_test(f);
    CHECK(link->count(protocol::CMD_G29) == 2);

    // tryb bez G29 odlacza kierownice, gdy plytka zglasza podlaczona
    cfg.output.mode = "none";
    cfg.output.hide_unused = true;
    eng.publish_config();
    eng.handle_state_for_test(f);
    CHECK(link->count(protocol::CMD_G29_USB) == 1);

    // srodek: offset zerowany, stare ramki nie psuja zakresu
    cfg.steer.center_counts = 77;
    eng.publish_config();
    eng.center_wheel();
    CHECK(cfg.steer.center_counts == 0);
    CHECK(eng.live()->steer.center_counts == 0);
    CHECK(link->count(protocol::CMD_CENTER) == 1);

    // kalibracja z zaobserwowanego zakresu
    eng.reset_raw_tracking();
    auto a = frame_with(0);
    a.axes[0] = 200;
    a.host_time = monotonic() + 1.0;
    eng.handle_state_for_test(a);
    a.axes[0] = 3000;
    eng.handle_state_for_test(a);
    CHECK(eng.calibrate_pedal_from_seen("throttle"));
    CHECK(cfg.pedals[0].raw_min == 200 && cfg.pedals[0].raw_max >= 3000);

    // FFB: lokalna sprezyna, bez plytki watek nie jest potrzebny
    cfg.ffb.enabled = true;
    cfg.ffb.overall_gain = 1.0;
    cfg.ffb.deadband = 0.0;
    cfg.ffb.spring_gain = 0.5;
    eng.publish_config();
    auto spring = frame_with(0);
    spring.host_time = monotonic() + 1.0;
    eng.handle_state_for_test(spring);
    double torque = 1.0;
    eng.compute_for_test(torque);
    CHECK(near_eq(torque, 0.0));
    cfg.ffb.enabled = false;
    eng.publish_config();
}

// ---------------------------------------------------------------- winsys

static void test_winsys() {
    auto g = winsys::parse_geometry("1240x880+40+-10");
    CHECK(g && g->w == 1240 && g->h == 880 && g->x == 40 && g->y == -10);
    CHECK(!winsys::parse_geometry("0x10+0+0"));
    CHECK(!winsys::parse_geometry("abc"));
    CHECK(!winsys::parse_geometry("10x10+0"));
    CHECK(winsys::covers({0, 0, 100, 100}, {1, 1, 99, 99}));
    CHECK(!winsys::covers({10, 0, 100, 100}, {0, 0, 99, 99}));
    CHECK(winsys::autostart_target(L"\"C:\\a b\\panel.exe\" --minimized") == L"C:\\a b\\panel.exe");
    CHECK(winsys::autostart_target(L"\"C:\\pythonw.exe\" \"C:\\k.py\" --minimized") == L"C:\\k.py");

    const wchar_t* key = L"Software\\KierownicaPanelCppTest";
    CHECK(winsys::autostart_set(L"\"x.exe\" --minimized", key, L"T"));
    CHECK(winsys::autostart_get(key, L"T") == std::wstring(L"\"x.exe\" --minimized"));
    CHECK(winsys::autostart_set(std::nullopt, key, L"T"));
    CHECK(!winsys::autostart_get(key, L"T"));
    RegDeleteKeyW(HKEY_CURRENT_USER, key);
}

// ------------------------------------------------- zgodnosc z wersja Python

static double num(const json::Value& v) { return v.to_double(); }

static void check_same(const char* what, const std::string& id, bool ok) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("GOLDEN FAIL %s [%s]\n", what, id.c_str());
    }
}

// Kolejnosc kluczy nie ma znaczenia - Python zachowuje kolejnosc z pliku.
static json::Value canon(const json::Value& v) {
    using json::Value;
    if (v.is_array()) {
        Value out = Value::array();
        for (const Value& item : v.items()) out.push(canon(item));
        return out;
    }
    if (v.is_object()) {
        std::vector<std::pair<std::string, const Value*>> sorted;
        for (const auto& [key, item] : v.members()) sorted.emplace_back(key, &item);
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        Value out = Value::object();
        for (const auto& [key, item] : sorted) out.set(key, canon(*item));
        return out;
    }
    return v;
}

static void test_golden(const fs::path& path) {
    using json::Value;
    Value root = json::parse(read_text(path));

    for (const Value& c : root.find("frames")->items()) {
        const std::string id = c.find("id")->as_string();
        Bytes in;
        for (const Value& b : c.find("bytes")->items()) in.push_back(static_cast<uint8_t>(b.as_int()));
        Bytes expect;
        for (const Value& b : c.find("expect")->items()) expect.push_back(static_cast<uint8_t>(b.as_int()));
        Bytes got;
        const std::string cmd = c.find("cmd")->as_string();
        const auto& a = c.find("args")->items();
        auto ll = [&](size_t i) { return static_cast<long long>(a[i].to_int(0)); };
        if (cmd == "ping") got = protocol::cmd_ping(static_cast<uint32_t>(ll(0)));
        else if (cmd == "endstop") got = protocol::cmd_set_endstop(ll(0), ll(1));
        else if (cmd == "rate") got = protocol::cmd_set_rate(ll(0));
        else if (cmd == "ffb") got = protocol::cmd_ffb(ll(0));
        else if (cmd == "usb") got = protocol::cmd_g29_usb(a[0].truthy());
        else if (cmd == "ffbcfg") got = protocol::cmd_ffb_config(a[0].truthy(), ll(1), ll(2), a[3].truthy(), ll(4), ll(5), ll(6));
        check_same("frame", id, got == expect);
    }

    for (const Value& c : root.find("configs")->items()) {
        const std::string id = c.find("id")->as_string();
        TempDir dir;
        fs::path p = dir.path / "config.json";
        write_text(p, c.find("input")->as_string());
        AppConfig cfg = AppConfig::load(p);
        std::string mine = json::dump(canon(json::parse(cfg.to_json())));
        std::string theirs = json::dump(canon(*c.find("output")));
        check_same("config", id, mine == theirs);
        if (mine != theirs) {
            size_t k = 0;
            while (k < mine.size() && k < theirs.size() && mine[k] == theirs[k]) ++k;
            size_t from = k > 80 ? k - 80 : 0;
            std::printf("  C++:    ...%s\n  Python: ...%s\n", mine.substr(from, 140).c_str(), theirs.substr(from, 140).c_str());
        }
    }

    AppConfig base_defaults;
    for (const Value& c : root.find("states")->items()) {
        const std::string id = c.find("id")->as_string();
        TempDir dir;
        fs::path p = dir.path / "config.json";
        write_text(p, c.find("config")->as_string());
        AppConfig cfg = AppConfig::load(p);
        StateProcessor proc;
        const auto& frames = c.find("frames")->items();
        const auto& results = c.find("results")->items();
        for (size_t i = 0; i < frames.size(); ++i) {
            const Value& fr = frames[i];
            protocol::StateFrame f;
            f.steer_counts = static_cast<int32_t>(fr.find("counts")->as_int());
            f.buttons = static_cast<uint32_t>(fr.find("buttons")->as_int());
            const auto& ax = fr.find("axes")->items();
            for (size_t k = 0; k < 6; ++k) f.axes[k] = static_cast<uint16_t>(ax[k].as_int());
            ProcessedState s = proc.process(f, cfg, num(*fr.find("t")));

            const Value& r = results[i];
            std::string at = id + "#" + std::to_string(i);
            check_same("steer_deg", at, near_eq(s.steer_deg, num(*r.find("deg")), 1e-9));
            check_same("steer_axis", at, near_eq(s.steer_axis, num(*r.find("axis")), 1e-9));
            check_same("velocity", at, near_eq(s.steer_velocity, num(*r.find("vel")), 1e-6));
            check_same("effective", at, s.buttons_effective == static_cast<uint32_t>(r.find("eff")->as_int()));
            check_same("hat", at, s.hat == r.find("hat")->as_int());
            const auto& peds = r.find("pedals")->items();
            for (size_t k = 0; k < NUM_PEDALS; ++k)
                check_same("pedal", at, near_eq(s.pedals[k], num(peds[k]), 1e-9));
            const auto& out = r.find("out")->items();
            bool same = static_cast<size_t>(s.out_buttons_count) == out.size();
            for (size_t k = 0; same && k < out.size(); ++k) same = s.out_buttons[k] == out[k].as_int();
            check_same("out_buttons", at, same);

            auto g = build_g29_report(s, cfg);
            const auto& g_expect = r.find("g29")->items();
            same = g_expect.size() == g.size();
            for (size_t k = 0; same && k < g.size(); ++k) same = g[k] == g_expect[k].as_int();
            check_same("g29", at, same);

            auto pad = build_pad_state(s, cfg);
            const auto& e = r.find("pad")->items();
            check_same("pad", at, static_cast<long long>(pad.buttons) == e[0].as_int() && pad.lt == e[1].as_int() &&
                                      pad.rt == e[2].as_int() && pad.lx == e[3].as_int() && pad.ly == e[4].as_int() &&
                                      pad.rx == e[5].as_int() && pad.ry == e[6].as_int());
        }
    }
    (void)base_defaults;
}

// Plytki: nazwy z HELLO, co ktora umie, DTR i czas oczekiwania na porcie.
static void test_boards() {
    protocol::HelloInfo h;
    h.fw_major = protocol::FW_LATEST_MAJOR;
    h.fw_minor = protocol::FW_LATEST_MINOR;
    h.name = "KIEROWNICA-UNO";
    h.caps = protocol::CAP_FFB;
    CHECK(h.board() == "Arduino Uno / Nano");
    CHECK(h.g29_unsupported());
    h.fw_major = 2;  // stary firmware bez G29 - to nie wina plytki
    CHECK(!h.g29_unsupported());

    h.fw_major = protocol::FW_LATEST_MAJOR;
    h.name = "KIEROWNICA-LEONARDO";
    h.caps = protocol::CAP_FFB | protocol::CAP_G29 | protocol::CAP_NATIVE_USB;
    CHECK(h.g29_same_cable() && !h.can_hide_g29() && !h.g29_unsupported());
    h.name = "KIEROWNICA-DUE";
    h.caps = protocol::CAP_FFB | protocol::CAP_G29 | protocol::CAP_G29_HIDE;
    CHECK(!h.g29_same_cable() && h.board() == "Arduino Due");
    h.name = "Due";  // nazwa spoza listy zostaje jak jest
    CHECK(h.board() == "Due");

    CHECK(protocol::board_by_key("esp32s3") && std::string(protocol::board_by_key("esp32s3")->hello) == "KIEROWNICA-ESP32S3");
    CHECK(protocol::board_by_key("xyz") == nullptr);
    CHECK(std::string(protocol::board_by_hello("KIEROWNICA-UNO")->key) == "uno");
    for (const auto& b : protocol::BOARDS) CHECK(protocol::board_by_key(b.key) == &b);

    // DTR tylko dla natywnego USB - mostki USB-UART resetuja plytke
    CHECK(wants_dtr(PortInfo{"COM5", "", "USB\\VID_2341&PID_8036&REV_0100&MI_00"}));
    CHECK(wants_dtr(PortInfo{"COM6", "", "USB\\VID_046D&PID_C24F&MI_00"}));
    CHECK(wants_dtr(PortInfo{"COM7", "", "USB\\VID_303A&PID_1001"}));
    CHECK(!wants_dtr(PortInfo{"COM3", "", "USB\\VID_2341&PID_003D&REV_0001"}));
    CHECK(!wants_dtr(PortInfo{"COM4", "", "USB\\VID_1A86&PID_7523&REV_0264"}));
    CHECK(probe_timeout(PortInfo{"COM4", "", "USB\\VID_1A86&PID_7523"}) == PROBE_TIMEOUT_BRIDGE);
    CHECK(probe_timeout(PortInfo{"COM4", "", "usb\\vid_10c4&pid_ea60"}) == PROBE_TIMEOUT_BRIDGE);
    CHECK(probe_timeout(PortInfo{"COM3", "", "USB\\VID_2341&PID_003D"}) == PROBE_TIMEOUT);
    CHECK(looks_like_arduino(PortInfo{"COM8", "Silicon Labs CP210x USB to UART Bridge (COM8)", ""}));

    // nieznana plytka w config.json wraca do "automatycznie"
    AppConfig cfg;
    cfg.link.board = "esp32";
    cfg.normalize();
    CHECK(cfg.link.board == "esp32");
    cfg.link.board = "atari";
    cfg.normalize();
    CHECK(cfg.link.board.empty());
}

// Aktualizacje: wersje, odpowiedz GitHuba, sumy kontrolne - bez sieci.
static void test_updater() {
    using namespace update;
    CHECK(compare_versions("3.2", "3.2") == 0);
    CHECK(compare_versions("v3.3", "3.2") > 0);
    CHECK(compare_versions("3.10", "3.9") > 0);   // liczbowo, nie alfabetycznie
    CHECK(compare_versions("3.2", "3.2.1") < 0);
    CHECK(compare_versions("3.2.0", "3.2") == 0);
    CHECK(compare_versions("4.0-beta", "3.9") > 0);
    // wersja programu = pelny numer z VERSIONINFO (i z tagu wydania) - inaczej
    // np. 3.2.1 przedstawialaby sie jako 3.2 i aktualizowala w kolko
    CHECK(compare_versions(APP_VERSION_TEXT, APP_VERSION_FULL) == 0);

    const char* api = R"({"tag_name": "v3.3", "name": "Kierownica 3.3", "html_url": "https://github.com/rybainformatyk/kierownica/releases/tag/v3.3",
        "body": "## Nowe\r\n* zasobnik", "assets": [
        {"name": "Kierownica-3.3-instalator.exe", "size": 17000000, "browser_download_url": "https://github.com/rybainformatyk/kierownica/releases/download/v3.3/Kierownica-3.3-instalator.exe"},
        {"name": "Kierownica.exe", "size": 2000000, "browser_download_url": "https://github.com/rybainformatyk/kierownica/releases/download/v3.3/Kierownica.exe"},
        {"name": "SHA256SUMS.txt", "size": 300, "browser_download_url": "https://github.com/rybainformatyk/kierownica/releases/download/v3.3/SHA256SUMS.txt"}]})";
    std::string error;
    auto rel = parse_release(api, error);
    CHECK(rel.has_value());
    if (rel) {
        CHECK(rel->version == "3.3" && rel->tag == "v3.3" && rel->assets.size() == 3);
        CHECK(rel->installer() && rel->installer()->name == "Kierownica-3.3-instalator.exe" && rel->installer()->size == 17000000);
        CHECK(rel->asset(EXE_ASSET) && rel->asset(SUMS_ASSET));
        CHECK(rel->notes.find("zasobnik") != std::string::npos);
    }
    CHECK(!parse_release("{\"message\": \"Not Found\"}", error));
    CHECK(!parse_release("to nie json", error));

    auto sums = parse_sums("89220A7865076B342892F98865F3499FB7C4CFD673159E89D352C360FD014C6A  Kierownica.exe\r\n"
                           "zle  linia\n"
                           "ef569a3105cd301b89580f18f60c66b339e95296acf2c0dfcaf4b4bbf8ab68fe *Kierownica-3.3-instalator.exe\n");
    CHECK(sums.size() == 2);
    CHECK(sums["Kierownica.exe"] == "89220a7865076b342892f98865f3499fb7c4cfd673159e89d352c360fd014c6a");
    CHECK(sums.contains("Kierownica-3.3-instalator.exe"));

    CHECK(trusted_url("https://github.com/rybainformatyk/kierownica/releases/download/v3.3/Kierownica.exe"));
    CHECK(!trusted_url("http://github.com/rybainformatyk/kierownica/releases/download/v3.3/Kierownica.exe"));
    CHECK(!trusted_url("https://github.com/ktos/inny/releases/download/v3.3/Kierownica.exe"));
    CHECK(!trusted_url("https://evil.example/rybainformatyk/kierownica/releases/download/x"));

    TempDir dir;
    write_text(dir.path / "abc.txt", "abc");
    CHECK(sha256_file(dir.path / "abc.txt") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_file(dir.path / "brak.txt").empty());
    CHECK(install_kind(dir.path) == InstallKind::Portable);
    write_text(dir.path / "unins000.exe", "x");
    CHECK(install_kind(dir.path) == InstallKind::Installer);

    // ustawienie sprawdzania przy starcie przetrwa zapis i odczyt
    AppConfig cfg;
    cfg.ui.updates = false;
    cfg.ui.tray = false;
    fs::path path = dir.path / "config.json";
    cfg.save(path);
    AppConfig back = AppConfig::load(path);
    CHECK(!back.ui.updates && !back.ui.tray);
}

// Prawdziwe zapytanie do GitHuba - tylko z KIEROWNICA_LIVE=1 (zwykle testy nie
// potrzebuja sieci). Udajemy stara wersje, wiec najnowsze wydanie jest "nowsze".
static void test_updater_live() {
    wchar_t flag[8] = {};
    if (!GetEnvironmentVariableW(L"KIEROWNICA_LIVE", flag, 8) || flag[0] != L'1') return;
    update::Updater u("0.1", update::InstallKind::Portable);
    u.check(true);
    update::State s;
    for (int i = 0; i < 300; ++i) {  // do 30 s
        s = u.state();
        if (s.phase != update::State::Phase::Checking) break;
        Sleep(100);
    }
    std::printf("live: faza %d, %s, wersja %s, mozna instalowac: %s\n", static_cast<int>(s.phase), s.message.c_str(),
                s.release ? s.release->version.c_str() : "-", s.can_install ? "tak" : "nie");
    CHECK(s.phase == update::State::Phase::Available);
    CHECK(s.release && !s.release->assets.empty());

    // pobieranie z przekierowaniem github.com -> serwer plikow; suma z `gh release download`
    TempDir dir;
    std::string error;
    uint64_t seen = 0;
    bool ok = update::download_file("https://github.com/rybainformatyk/kierownica/releases/download/v3.1/Kierownica.exe",
                                    dir.path / "Kierownica.exe", [&](uint64_t done, uint64_t) { seen = done; return true; }, error);
    std::printf("live: pobrano %llu B %s\n", static_cast<unsigned long long>(seen), error.c_str());
    CHECK(ok);
    CHECK(update::sha256_file(dir.path / "Kierownica.exe") == "ebd019abf2c0d016002188731233af6fe5579d71240f28bacef4b665cfa325f9");
    CHECK(!fs::exists(dir.path / "Kierownica.exe.part"));
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    test_protocol();
    test_json();
    test_config();
    test_processing();
    test_ffb();
    test_engine();
    test_winsys();
    test_boards();
    test_updater();
    test_updater_live();
    if (argc > 1) test_golden(argv[1]);
    std::printf("%d sprawdzen, %d bledow\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}

#include "xinput.h"

#include <windows.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <set>

#include "../embedded.h"
#include "../util.h"

namespace fs = std::filesystem;

namespace wb {

namespace {

constexpr const wchar_t* DLL_NAME = L"ViGEmClient.dll";
constexpr double PRIME_DELAY = 0.03;    // przerwa miedzy raportami inicjalizujacymi pada
constexpr double PRIME_TIMEOUT = 3.0;   // ile najwyzej czekamy, az Windows wyliczy pada
constexpr unsigned VIGEM_ERROR_NONE = 0x20000000;

#ifdef _WIN64
constexpr const wchar_t* ARCH = L"x64";
constexpr int BITS = 64;
#else
constexpr const wchar_t* ARCH = L"x86";
constexpr int BITS = 32;
#endif

// Raport zgodny z XINPUT_GAMEPAD (uklad pol wg naglowka ViGEm).
struct XUSB_REPORT {
    USHORT wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
};

struct XINPUT_STATE_ {
    DWORD dwPacketNumber;
    XUSB_REPORT Gamepad;
};

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE_*);

// XInput sluzy nam tylko do weryfikacji stanu startowego pada.
XInputGetStateFn xinput() {
    static XInputGetStateFn fn = [] () -> XInputGetStateFn {
        for (const wchar_t* name : {L"XInput1_4.dll", L"XInput1_3.dll", L"XInput9_1_0.dll"}) {
            HMODULE h = LoadLibraryW(name);
            if (!h) continue;
            if (auto f = reinterpret_cast<XInputGetStateFn>(GetProcAddress(h, "XInputGetState"))) return f;
        }
        return nullptr;
    }();
    return fn;
}

// Maska zajetych gniazd XInput (0-3).
unsigned xinput_slots() {
    auto fn = xinput();
    if (!fn) return 0;
    unsigned out = 0;
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE_ st{};
        if (fn(i, &st) == 0) out |= 1u << i;
    }
    return out;
}

// True gdy gniazdo raportuje same zera. nullopt gdy nie da sie odczytac.
std::optional<bool> xinput_is_zeroed(int slot) {
    auto fn = xinput();
    if (!fn) return std::nullopt;
    XINPUT_STATE_ st{};
    if (fn(static_cast<DWORD>(slot), &st) != 0) return std::nullopt;
    const XUSB_REPORT& g = st.Gamepad;
    return g.wButtons == 0 && g.bLeftTrigger == 0 && g.bRightTrigger == 0 && g.sThumbLX == 0 &&
           g.sThumbLY == 0 && g.sThumbRX == 0 && g.sThumbRY == 0;
}

std::string err(unsigned code) {
    switch (code) {
    case 0x20000000: return "OK";
    case 0xE0000001: return "nie znaleziono sterownika ViGEmBus - zainstaluj go";
    case 0xE0000002: return "brak wolnego gniazda na kolejne urządzenie";
    case 0xE0000003: return "nieprawidłowy cel";
    case 0xE0000004: return "nie udało się usunąć urządzenia";
    case 0xE0000005: return "urządzenie jest już podłączone";
    case 0xE0000006: return "urządzenie niezainicjowane";
    case 0xE0000007: return "urządzenie nie jest podłączone";
    case 0xE0000008: return "niezgodna wersja sterownika ViGEmBus - zaktualizuj go";
    case 0xE0000009: return "brak dostępu do sterownika (spróbuj uruchomić jako administrator)";
    case 0xE0000010: return "callback już zarejestrowany";
    case 0xE0000011: return "nie znaleziono callbacku";
    case 0xE0000012: return "magistrala już połączona";
    case 0xE0000013: return "nieprawidłowy uchwyt magistrali";
    case 0xE0000014: return "numer użytkownika poza zakresem";
    case 0xE0000015: return "nieprawidłowy parametr";
    case 0xE0000016: return "operacja nieobsługiwana";
    default: return std::format("nieznany błąd 0x{:08X}", code);
    }
}

// Funkcje ViGEmClient.dll.
struct ViGEmApi {
    void*(__cdecl* vigem_alloc)() = nullptr;
    void(__cdecl* vigem_free)(void*) = nullptr;
    unsigned(__cdecl* vigem_connect)(void*) = nullptr;
    void(__cdecl* vigem_disconnect)(void*) = nullptr;
    void*(__cdecl* vigem_target_x360_alloc)() = nullptr;
    void(__cdecl* vigem_target_free)(void*) = nullptr;
    unsigned(__cdecl* vigem_target_add)(void*, void*) = nullptr;
    unsigned(__cdecl* vigem_target_remove)(void*, void*) = nullptr;
    unsigned(__cdecl* vigem_target_x360_update)(void*, void*, XUSB_REPORT) = nullptr;
};

template <typename F>
void bind(HMODULE dll, F& fn, const char* name) {
    fn = reinterpret_cast<F>(GetProcAddress(dll, name));
    if (!fn) throw ViGEmNotAvailable(std::string("ViGEmClient.dll nie ma funkcji ") + name);
}

// Biblioteka zostaje zaladowana do konca procesu.
ViGEmApi* load_api(const fs::path& path) {
    static std::mutex m;
    static std::map<std::wstring, ViGEmApi> cache;
    std::lock_guard lock(m);
    auto it = cache.find(path.wstring());
    if (it != cache.end()) return &it->second;

    HMODULE d = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!d) {
        throw ViGEmNotAvailable(
            "Nie udało się załadować " + narrow(path.wstring()) + ":\n" + win_error_text(GetLastError()) +
            std::format("\n\nNajczęstsza przyczyna to niezgodność architektury - panel jest {}-bitowy, "
                        "więc potrzebna jest biblioteka z katalogu {}.", BITS, BITS == 64 ? "x64" : "x86"));
    }
    ViGEmApi api;
    bind(d, api.vigem_alloc, "vigem_alloc");
    bind(d, api.vigem_free, "vigem_free");
    bind(d, api.vigem_connect, "vigem_connect");
    bind(d, api.vigem_disconnect, "vigem_disconnect");
    bind(d, api.vigem_target_x360_alloc, "vigem_target_x360_alloc");
    bind(d, api.vigem_target_free, "vigem_target_free");
    bind(d, api.vigem_target_add, "vigem_target_add");
    bind(d, api.vigem_target_remove, "vigem_target_remove");
    bind(d, api.vigem_target_x360_update, "vigem_target_x360_update");
    return &cache.emplace(path.wstring(), api).first->second;
}

ViGEmApi* g_api = nullptr;

std::vector<fs::path> candidate_dirs() {
    std::vector<fs::path> dirs;
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(L"VIGEM_DIR", buf, 32768);
    if (n > 0 && n < 32768) dirs.emplace_back(buf);

    // biblioteka wbudowana w program - podstawowa sciezka
    if (auto embedded_dir = embedded::vigem_dir()) dirs.push_back(*embedded_dir);

    fs::path app = exe_dir();
    dirs.push_back(app / L"vigem" / ARCH);
    dirs.push_back(app / L"vigem");
    dirs.push_back(app);

    for (const wchar_t* name : {L"ProgramFiles", L"ProgramW6432"}) {
        n = GetEnvironmentVariableW(name, buf, 32768);
        std::wstring base = (n > 0 && n < 32768) ? std::wstring(buf, n) : L"C:\\Program Files";
        dirs.push_back(fs::path(base) / L"Nefarius Software Solutions" / L"ViGEm Bus Driver");
    }
    return dirs;
}

SHORT clamp_s16(int v) { return static_cast<SHORT>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }
BYTE clamp_u8(int v) { return static_cast<BYTE>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

struct XusbEntry { const char* name; unsigned mask; };
constexpr XusbEntry XUSB[] = {
    {"DPAD_UP", 0x0001}, {"DPAD_DOWN", 0x0002}, {"DPAD_LEFT", 0x0004}, {"DPAD_RIGHT", 0x0008},
    {"START", 0x0010},   {"BACK", 0x0020},      {"LS", 0x0040},        {"RS", 0x0080},
    {"LB", 0x0100},      {"RB", 0x0200},        {"GUIDE", 0x0400},     {"A", 0x1000},
    {"B", 0x2000},       {"X", 0x4000},         {"Y", 0x8000},
};

} // namespace

unsigned xusb_mask(std::string_view name) {
    for (const auto& e : XUSB)
        if (name == e.name) return e.mask;
    return 0;
}

const std::map<std::string, std::string> PAD_BUTTON_LABELS = {
    {"A", "A"}, {"B", "B"}, {"X", "X"}, {"Y", "Y"},
    {"LB", "LB"}, {"RB", "RB"}, {"LS", "L3"}, {"RS", "R3"},
    {"BACK", "Back"}, {"START", "Start"}, {"GUIDE", "Guide"},
    {"DPAD_UP", "Krzyżak góra"}, {"DPAD_DOWN", "Krzyżak dół"},
    {"DPAD_LEFT", "Krzyżak lewo"}, {"DPAD_RIGHT", "Krzyżak prawo"}};

std::vector<std::string> PadState::pressed_names() const {
    std::vector<std::string> out;
    for (const auto& e : XUSB)
        if (buttons & e.mask) out.emplace_back(e.name);
    return out;
}

std::optional<fs::path> find_vigem_dll() {
    for (const auto& d : candidate_dirs()) {
        fs::path candidate = d / DLL_NAME;
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return std::nullopt;
}

ViGEmDiagnostics vigem_diagnostics() {
    ViGEmDiagnostics info;
    info.bits = BITS;
    auto path = find_vigem_dll();
    if (!path) {
        info.error = "Nie znaleziono ViGEmClient.dll. Nie udało się rozpakować go z programu do %LOCALAPPDATA%\\Kierownica.";
        return info;
    }
    info.dll_path = narrow(path->wstring());
    ViGEmApi* api = nullptr;
    try {
        api = load_api(*path);
    } catch (const std::exception& exc) {
        info.error = std::string("Nie udało się załadować biblioteki: ") + exc.what();
        return info;
    }
    info.dll_loaded = true;

    void* client = api->vigem_alloc();
    if (!client) {
        info.error = "vigem_alloc() nie przydzielił pamięci";
        return info;
    }
    unsigned code = api->vigem_connect(client);
    if (code == VIGEM_ERROR_NONE) {
        info.driver_ok = true;
        api->vigem_disconnect(client);
    } else {
        info.error = err(code);
    }
    api->vigem_free(client);
    return info;
}

void XInputBackend::open() {
    if (open_) return;
    auto path = find_vigem_dll();
    if (!path) {
        throw ViGEmNotAvailable("Nie znaleziono ViGEmClient.dll.\n\n"
                                "Nie udało się rozpakować jej z programu do %LOCALAPPDATA%\\Kierownica.");
    }
    ViGEmApi* api = load_api(*path);

    void* client = api->vigem_alloc();
    if (!client) throw ViGEmNotAvailable("Nie udało się przydzielić klienta ViGEm.");

    unsigned code = api->vigem_connect(client);
    if (code != VIGEM_ERROR_NONE) {
        api->vigem_free(client);
        throw ViGEmNotAvailable("Nie udało się połączyć ze sterownikiem ViGEmBus:\n" + err(code) +
                                "\n\nZainstaluj ViGEmBus:\nhttps://github.com/nefarius/ViGEmBus/releases");
    }

    void* target = api->vigem_target_x360_alloc();
    if (!target) {
        api->vigem_disconnect(client);
        api->vigem_free(client);
        throw ViGEmNotAvailable("Nie udało się utworzyć wirtualnego pada.");
    }

    std::optional<unsigned> slots_before;
    if (xinput()) slots_before = xinput_slots();
    code = api->vigem_target_add(client, target);
    if (code != VIGEM_ERROR_NONE) {
        api->vigem_target_free(target);
        api->vigem_disconnect(client);
        api->vigem_free(client);
        throw ViGEmNotAvailable("Nie udało się podłączyć wirtualnego pada:\n" + err(code));
    }

    g_api = api;
    client_ = client;
    target_ = target;
    open_ = true;
    last_.reset();
    prime(slots_before);
}

void XInputBackend::close() {
    if (!open_ || !g_api) {
        open_ = false;
        return;
    }
    center();
    if (target_) {
        g_api->vigem_target_remove(client_, target_);
        g_api->vigem_target_free(target_);
    }
    if (client_) {
        g_api->vigem_disconnect(client_);
        g_api->vigem_free(client_);
    }
    client_ = nullptr;
    target_ = nullptr;
    open_ = false;
    last_.reset();
    slot_.reset();
}

void XInputBackend::nudge_then_zero() {
    XUSB_REPORT nudge{};
    nudge.sThumbLX = 1;
    g_api->vigem_target_x360_update(client_, target_, nudge);
    precise_sleep(PRIME_DELAY);
    last_.reset();
    center();
    precise_sleep(PRIME_DELAY);
}

// Doprowadza swiezo podlaczonego pada do stanu wyzerowanego.
//
// ViGEmBus wypycha raport dopiero wtedy, gdy rozni sie on od zapamietanego -
// a zapamietany startowo jest zerowy. Zapis samych zer zaraz po dodaniu
// urzadzenia niczego nie wysyla, wiec XInput czyta z drazkow nieokreslone
// wartosci (okolo -3300) - w grze auto skrecaloby samo. Lekarstwem jest jeden
// niezerowy raport, a po nim zera. Dopoki Windows nie wyliczy urzadzenia,
// raporty przepadaja, wiec czytamy stan z powrotem przez XInput i powtarzamy.
void XInputBackend::prime(std::optional<unsigned> slots_before) {
    if (!open_ || !g_api) return;
    nudge_then_zero();

    if (!xinput() || !slots_before) return;  // bez XInputa zostaje sama sekwencja

    double deadline = monotonic() + PRIME_TIMEOUT;
    while (monotonic() < deadline) {
        unsigned fresh = xinput_slots() & ~*slots_before;
        if (fresh) {
            int slot = 0;
            while (!((fresh >> slot) & 1)) ++slot;
            slot_ = slot;
            if (xinput_is_zeroed(slot).value_or(false)) return;
            nudge_then_zero();
        } else {
            precise_sleep(0.05);
        }
    }
}

void XInputBackend::send(const PadState& state, bool force) {
    if (!open_ || !g_api) return;
    if (last_ && *last_ == state && !force) return;
    last_ = state;

    XUSB_REPORT report{};
    report.wButtons = static_cast<USHORT>(state.buttons & 0xFFFF);
    report.bLeftTrigger = clamp_u8(state.lt);
    report.bRightTrigger = clamp_u8(state.rt);
    report.sThumbLX = clamp_s16(state.lx);
    report.sThumbLY = clamp_s16(state.ly);
    report.sThumbRX = clamp_s16(state.rx);
    report.sThumbRY = clamp_s16(state.ry);
    g_api->vigem_target_x360_update(client_, target_, report);
}

std::string XInputBackend::status_text() const {
    if (!open_) return "Pad nieaktywny";
    std::string where = slot_ ? std::format(" (gniazdo {})", *slot_ + 1) : "";
    return "Pad Xbox 360 podłączony" + where + " - gry widzą go jako kontroler XInput";
}

} // namespace wb

#include "vjoy.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <format>
#include <mutex>
#include <set>

#include "../util.h"

namespace fs = std::filesystem;

namespace wb {

namespace {

constexpr const wchar_t* DLL_NAME = L"vJoyInterface.dll";
constexpr const wchar_t* CONFIG_EXE = L"vJoyConfig.exe";
constexpr DWORD ERROR_CANCELLED_UAC = 1223;

#ifdef _WIN64
constexpr const wchar_t* ARCH = L"x64";
constexpr int BITS = 64;
#else
constexpr const wchar_t* ARCH = L"x86";
constexpr int BITS = 32;
#endif

// HID usage ID osi - zgodne z public.h z SDK vJoy
constexpr std::array<UINT, NUM_AXIS_KEYS> HID_USAGE = {0x30, 0x31, 0x32, 0x33, 0x34,
                                                       0x35, 0x36, 0x37, 0x38};

std::wstring env(const wchar_t* name, const wchar_t* fallback = L"") {
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return (n > 0 && n < 32768) ? std::wstring(buf, n) : std::wstring(fallback);
}

std::wstring reg_string(HKEY key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size) != ERROR_SUCCESS)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ) return {};
    return std::wstring(buf, wcsnlen(buf, size / sizeof(wchar_t)));
}

std::vector<fs::path> registry_dirs() {
    std::vector<fs::path> out;
    for (const wchar_t* path : {L"SOFTWARE\\Shaul Eizikovich\\vJoy",
                                L"SOFTWARE\\WOW6432Node\\Shaul Eizikovich\\vJoy"}) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &k) != ERROR_SUCCESS) continue;
        std::wstring loc = reg_string(k, L"InstallLocation");
        RegCloseKey(k);
        if (!loc.empty()) {
            out.emplace_back(loc);
            out.push_back(fs::path(loc) / ARCH);
        }
    }
    // wpisy odinstalowywania
    HKEY root;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                      KEY_READ, &root) == ERROR_SUCCESS) {
        wchar_t name[256];
        for (DWORD i = 0;; ++i) {
            DWORD len = 256;
            LONG rc = RegEnumKeyExW(root, i, name, &len, nullptr, nullptr, nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS) break;
            if (rc != ERROR_SUCCESS) continue;
            HKEY sub;
            if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
            std::wstring disp = reg_string(sub, L"DisplayName");
            std::transform(disp.begin(), disp.end(), disp.begin(), ::towlower);
            if (disp.find(L"vjoy") != std::wstring::npos) {
                std::wstring loc = reg_string(sub, L"InstallLocation");
                if (!loc.empty()) {
                    out.emplace_back(loc);
                    out.push_back(fs::path(loc) / ARCH);
                }
            }
            RegCloseKey(sub);
        }
        RegCloseKey(root);
    }
    return out;
}

std::vector<fs::path> candidate_dirs() {
    std::vector<fs::path> dirs;
    std::wstring e = env(L"VJOY_DIR");
    if (!e.empty()) dirs.emplace_back(e);

    for (const std::wstring& base : {env(L"ProgramFiles", L"C:\\Program Files"),
                                     env(L"ProgramFiles(x86)", L"C:\\Program Files (x86)"),
                                     env(L"ProgramW6432", L"C:\\Program Files")}) {
        fs::path root = fs::path(base) / L"vJoy";
        dirs.push_back(root / ARCH);
        dirs.push_back(root);
        dirs.push_back(root / L"x64");
        dirs.push_back(root / L"x86");
    }

    // obok aplikacji - przydatne przy wersji przenosnej
    fs::path here = exe_dir();
    dirs.push_back(here);
    dirs.push_back(here / L"vjoy");
    dirs.push_back(here / ARCH);

    auto reg = registry_dirs();
    dirs.insert(dirs.end(), reg.begin(), reg.end());

    std::set<std::wstring> seen;
    std::vector<fs::path> out;
    for (const auto& d : dirs) {
        std::wstring key = d.wstring();
        std::transform(key.begin(), key.end(), key.begin(), ::towlower);
        if (seen.insert(key).second) out.push_back(d);
    }
    return out;
}

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

template <typename F>
void bind_required(HMODULE dll, F& fn, const char* name) {
    fn = reinterpret_cast<F>(GetProcAddress(dll, name));
    if (!fn) throw VJoyNotAvailable(std::string("vJoyInterface.dll nie ma funkcji ") + name);
}

template <typename F>
void bind_optional(HMODULE dll, F& fn, const char* name) {
    fn = reinterpret_cast<F>(GetProcAddress(dll, name));
}

// Bibliotek nie zwalniamy - callback FFB zostaje w niej zarejestrowany.
std::mutex g_api_mutex;
std::map<std::wstring, std::unique_ptr<VJoyApi>> g_api_cache;

VJoyApi& get_api() {
    auto dll_path = find_vjoy_dll();
    if (!dll_path) {
        throw VJoyNotAvailable(
            "Nie znaleziono vJoyInterface.dll.\n\n"
            "Zainstaluj vJoy (vjoystick na SourceForge), a potem w programie vJoyConf włącz "
            "urządzenie nr 1 z osiami X, Y, Z, Rx, Ry, Rz, 32 przyciskami i włączonym Force Feedback.\n\n" +
            std::format("Uwaga: panel jest {}-bitowy, więc potrzebna jest biblioteka vJoy w tej samej architekturze.", BITS));
    }
    std::wstring key = dll_path->wstring();
    std::transform(key.begin(), key.end(), key.begin(), ::towlower);
    std::lock_guard lock(g_api_mutex);
    auto it = g_api_cache.find(key);
    if (it != g_api_cache.end()) return *it->second;
    auto api = std::make_unique<VJoyApi>(*dll_path);
    VJoyApi& ref = *api;
    g_api_cache.emplace(std::move(key), std::move(api));
    return ref;
}

bool is_admin() { return IsUserAnAdmin() != FALSE; }

// Uruchamia program z uprawnieniami administratora (UAC) i czeka na koniec.
// `hwnd` to okno panelu - okno UAC wyskakuje wtedy nad nim. nullopt, gdy
// uzytkownik odmowil albo program nie ruszyl; kod bledu trafia do `error`.
std::optional<DWORD> run_elevated(const fs::path& exe, const std::wstring& args, double timeout_s, HWND hwnd,
                                  DWORD& error) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = hwnd;
    info.lpVerb = L"runas";
    std::wstring file = exe.wstring();
    std::wstring dir = exe.parent_path().wstring();
    info.lpFile = file.c_str();
    info.lpParameters = args.c_str();
    info.lpDirectory = dir.c_str();
    info.nShow = SW_HIDE;
    // ShellExecuteEx potrafi siegac po rozszerzenia powloki przez COM, a wola
    // go watek przelaczania vJoy - Microsoft zaleca wtedy zainicjowac COM.
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    BOOL ok = ShellExecuteExW(&info);
    error = ok ? 0 : GetLastError();
    if (SUCCEEDED(com)) CoUninitialize();
    if (!ok || !info.hProcess) return std::nullopt;

    WaitForSingleObject(info.hProcess, static_cast<DWORD>(timeout_s * 1000));
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code;
}

bool run_hidden(const fs::path& exe, const std::wstring& args, double timeout_s, std::string& error) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = win_error_text(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    DWORD wait = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(timeout_s * 1000));
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        error = "przekroczono czas";
        return false;
    }
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace

const char* vjoy_status_text(int status) {
    switch (status) {
    case VJD_STAT_OWN: return "zajęte przez tę aplikację";
    case VJD_STAT_FREE: return "wolne";
    case VJD_STAT_BUSY: return "zajęte przez inny program";
    case VJD_STAT_MISS: return "nie istnieje - włącz je w vJoyConf";
    case VJD_STAT_UNKN: return "stan nieznany";
    default: return "?";
    }
}

const char* vjoy_axis_label(const std::string& key) {
    static const std::map<std::string, const char*> labels = {
        {"X", "X (kierownica)"}, {"Y", "Y"},      {"Z", "Z"},     {"RX", "Rx"},  {"RY", "Ry"},
        {"RZ", "Rz"},            {"SL0", "Slider"}, {"SL1", "Dial"}, {"WHL", "Wheel"}};
    auto it = labels.find(key);
    return it == labels.end() ? key.c_str() : it->second;
}

std::optional<fs::path> find_vjoy_dll() {
    for (const auto& d : candidate_dirs()) {
        fs::path candidate = d / DLL_NAME;
        if (is_file(candidate)) return candidate;
    }
    return std::nullopt;
}

std::optional<fs::path> find_vjoy_config() {
    std::vector<fs::path> dirs;
    if (auto dll = find_vjoy_dll()) dirs.push_back(dll->parent_path());
    auto rest = candidate_dirs();
    dirs.insert(dirs.end(), rest.begin(), rest.end());
    for (const auto& d : dirs) {
        fs::path candidate = d / CONFIG_EXE;
        if (is_file(candidate)) return candidate;
    }
    return std::nullopt;
}

std::optional<bool> vjoy_driver_enabled() {
    try {
        return get_api().vJoyEnabled() != 0;
    } catch (...) {
        return std::nullopt;
    }
}

std::pair<bool, std::string> set_vjoy_driver(bool enabled, double timeout_s, HWND hwnd) {
    auto exe = find_vjoy_config();
    if (!exe)
        return {false, std::string("nie znaleziono vJoyConfig.exe - ") + (enabled ? "włącz" : "wyłącz") +
                           " vJoy ręcznie w vJoyConf"};
    std::wstring args = std::wstring(L"enable ") + (enabled ? L"on" : L"off");
    if (is_admin()) {
        std::string error;
        if (!run_hidden(*exe, args, timeout_s, error)) return {false, "vJoyConfig.exe: " + error};
    } else {
        DWORD error = 0;
        if (!run_elevated(*exe, args, timeout_s, hwnd, error)) {
            if (error == ERROR_CANCELLED_UAC) return {false, "brak zgody administratora (okno UAC zamknięte)"};
            return {false, "nie udało się uruchomić vJoyConfig.exe"};
        }
    }

    // Urzadzenie pojawia sie / znika chwile po zakonczeniu vJoyConfig
    double deadline = monotonic() + 5.0;
    for (;;) {
        if (vjoy_driver_enabled() == enabled)
            return {true, std::string("vJoy ") + (enabled ? "włączone" : "wyłączone - gry go nie widzą")};
        if (monotonic() > deadline)
            return {false, "vJoyConfig nie zmienił stanu sterownika - możliwe, że trzeba zrestartować komputer"};
        Sleep(200);
    }
}

VJoyDiagnostics vjoy_diagnostics() {
    VJoyDiagnostics info;
    info.bits = BITS;
    auto dll_path = find_vjoy_dll();
    if (!dll_path) {
        info.error = std::format("Nie znaleziono vJoyInterface.dll. Zainstaluj vJoy (wersja {} musi odpowiadać panelowi).",
                                 BITS == 64 ? "x64" : "x86");
        return info;
    }
    info.dll_path = narrow(dll_path->wstring());
    VJoyApi* api = nullptr;
    try {
        api = &get_api();
    } catch (const std::exception& exc) {
        info.error = std::string("Nie udało się załadować DLL: ") + exc.what();
        return info;
    }

    info.dll_loaded = true;
    info.enabled = api->vJoyEnabled() != 0;
    info.version = api->GetvJoyVersion();
    if (void* ptr = api->GetvJoyProductString()) info.product = narrow(static_cast<const wchar_t*>(ptr));

    for (UINT rid = 1; rid <= 16; ++rid) {
        int status = api->GetVJDStatus(rid);
        if (status == VJD_STAT_MISS) continue;
        VJoyDeviceInfo d;
        d.id = static_cast<int>(rid);
        d.status = status;
        d.status_text = vjoy_status_text(status);
        d.buttons = api->GetVJDButtonNumber(rid);
        for (size_t i = 0; i < NUM_AXIS_KEYS; ++i)
            if (api->GetVJDAxisExist(rid, HID_USAGE[i])) d.axes.emplace_back(AXIS_KEYS[i]);
        d.disc_pov = api->GetVJDDiscPovNumber(rid);
        d.cont_pov = api->GetVJDContPovNumber(rid);
        d.ffb = api->IsDeviceFfb(rid) != 0;
        info.devices.push_back(std::move(d));
    }
    return info;
}

VJoyApi::VJoyApi(const fs::path& dll_path) {
    // Katalog DLL dodajemy do sciezki szukania, bo vJoyInterface moze ciagnac
    // za soba zaleznosci z tego samego folderu.
    HMODULE d = LoadLibraryExW(dll_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!d) {
        throw VJoyNotAvailable("Nie udało się załadować " + narrow(dll_path.wstring()) + ":\n" +
                               win_error_text(GetLastError()) +
                               std::format("\n\nNajczęstsza przyczyna to niezgodność architektury (panel {}-bit vs. biblioteka vJoy).", BITS));
    }
    bind_required(d, vJoyEnabled, "vJoyEnabled");
    bind_required(d, GetvJoyVersion, "GetvJoyVersion");
    bind_required(d, GetvJoyProductString, "GetvJoyProductString");
    bind_required(d, GetVJDStatus, "GetVJDStatus");
    bind_required(d, AcquireVJD, "AcquireVJD");
    bind_required(d, RelinquishVJD, "RelinquishVJD");
    bind_required(d, ResetVJD, "ResetVJD");
    bind_required(d, GetVJDButtonNumber, "GetVJDButtonNumber");
    bind_required(d, GetVJDDiscPovNumber, "GetVJDDiscPovNumber");
    bind_required(d, GetVJDContPovNumber, "GetVJDContPovNumber");
    bind_required(d, GetVJDAxisExist, "GetVJDAxisExist");
    bind_required(d, GetVJDAxisMax, "GetVJDAxisMax");
    bind_required(d, GetVJDAxisMin, "GetVJDAxisMin");
    bind_required(d, SetAxis, "SetAxis");
    bind_required(d, SetBtn, "SetBtn");
    bind_required(d, SetDiscPov, "SetDiscPov");
    bind_required(d, SetContPov, "SetContPov");
    bind_required(d, IsDeviceFfb, "IsDeviceFfb");

    bind_optional(d, FfbRegisterGenCB, "FfbRegisterGenCB");
    bind_optional(d, Ffb_h_DeviceID, "Ffb_h_DeviceID");
    bind_optional(d, Ffb_h_Type, "Ffb_h_Type");
    bind_optional(d, Ffb_h_EBI, "Ffb_h_EBI");
    bind_optional(d, Ffb_h_Eff_Report, "Ffb_h_Eff_Report");
    bind_optional(d, Ffb_h_EffOp, "Ffb_h_EffOp");
    bind_optional(d, Ffb_h_Eff_Constant, "Ffb_h_Eff_Constant");
    bind_optional(d, Ffb_h_Eff_Ramp, "Ffb_h_Eff_Ramp");
    bind_optional(d, Ffb_h_Eff_Period, "Ffb_h_Eff_Period");
    bind_optional(d, Ffb_h_Eff_Cond, "Ffb_h_Eff_Cond");
    bind_optional(d, Ffb_h_Eff_Envlp, "Ffb_h_Eff_Envlp");
    bind_optional(d, Ffb_h_DevCtrl, "Ffb_h_DevCtrl");
    bind_optional(d, Ffb_h_DevGain, "Ffb_h_DevGain");
}

std::vector<std::string> VJoyBackend::available_axes() const {
    std::vector<std::string> out;
    for (size_t i = 0; i < NUM_AXIS_KEYS; ++i)
        if (axes_[i].exists) out.emplace_back(AXIS_KEYS[i]);
    return out;
}

void VJoyBackend::open() {
    if (open_) return;
    VJoyApi& api = get_api();

    if (!api.vJoyEnabled()) {
        throw VJoyNotAvailable(
            "Sterownik vJoy jest zainstalowany, ale wyłączony.\n"
            "Uruchom vJoyConf i włącz vJoy, albo sprawdź w Menedżerze urządzeń, czy 'vJoy Device' nie ma błędu.");
    }

    version_ = api.GetvJoyVersion();
    void* ptr = api.GetvJoyProductString();
    product_ = ptr ? narrow(static_cast<const wchar_t*>(ptr)) : "";

    UINT rid = static_cast<UINT>(device_id_);
    int status = api.GetVJDStatus(rid);
    if (status == VJD_STAT_MISS)
        throw VJoyNotAvailable(std::format("Urządzenie vJoy #{} nie istnieje.\nOtwórz vJoyConf, zaznacz 'Device {}' i kliknij Apply.", rid, rid));
    if (status == VJD_STAT_BUSY)
        throw VJoyNotAvailable(std::format("Urządzenie vJoy #{} jest zajęte przez inny program.\n"
                                           "Zamknij pozostałe aplikacje korzystające z vJoy (np. drugie okno tego panelu, x360ce, FreePIE).", rid));
    if (status != VJD_STAT_FREE && status != VJD_STAT_OWN)
        throw VJoyNotAvailable(std::format("Urządzenie vJoy #{}: {}", rid, vjoy_status_text(status)));

    if (!api.AcquireVJD(rid))
        throw VJoyNotAvailable(std::format("Nie udało się przejąć urządzenia vJoy #{}. Spróbuj uruchomić panel jako administrator.", rid));

    api_ = &api;
    open_ = true;

    // odczyt mozliwosci urzadzenia
    for (size_t i = 0; i < NUM_AXIS_KEYS; ++i) {
        AxisRange a;
        if (api.GetVJDAxisExist(rid, HID_USAGE[i])) {
            a.exists = true;
            LONG lo = 0, hi = 32767;
            if (!api.GetVJDAxisMin(rid, HID_USAGE[i], &lo)) lo = 0;
            if (!api.GetVJDAxisMax(rid, HID_USAGE[i], &hi)) hi = 32767;
            if (hi <= lo) {
                lo = 0;
                hi = 32767;
            }
            a.lo = lo;
            a.hi = hi;
        }
        axes_[i] = a;
    }

    buttons_ = std::max(0, api.GetVJDButtonNumber(rid));
    disc_pov_ = std::max(0, api.GetVJDDiscPovNumber(rid));
    cont_pov_ = std::max(0, api.GetVJDContPovNumber(rid));
    ffb_capable_ = api.IsDeviceFfb(rid) != 0 && api.FfbRegisterGenCB != nullptr;

    api.ResetVJD(rid);
    pressed_.reset();
    last_hat_ = -2;
    center();
}

void VJoyBackend::close() {
    if (!open_ || !api_) {
        open_ = false;
        return;
    }
    center();
    api_->RelinquishVJD(static_cast<UINT>(device_id_));
    open_ = false;
    api_ = nullptr;
}

void VJoyBackend::center(const AxisValues& neutral) {
    if (!open_ || !api_) return;
    UINT rid = static_cast<UINT>(device_id_);
    for (size_t i = 0; i < NUM_AXIS_KEYS; ++i) {
        AxisRange& a = axes_[i];
        if (!a.exists) continue;
        LONG raw;
        if (neutral.set[i]) {
            raw = a.lo + static_cast<LONG>(py_round(clamp01(neutral.value[i]) * (a.hi - a.lo)));
        } else {
            raw = (a.lo + a.hi) / 2;
        }
        api_->SetAxis(raw, rid, HID_USAGE[i]);
        a.last = raw;
        a.has_last = true;
    }
    for (size_t b = 1; b < pressed_.size(); ++b)
        if (pressed_[b]) api_->SetBtn(0, rid, static_cast<UCHAR>(b));
    pressed_.reset();
    set_hat(-1);
}

void VJoyBackend::update(const AxisValues& axes, const uint8_t* buttons, int button_count, int hat) {
    if (!open_ || !api_) return;
    UINT rid = static_cast<UINT>(device_id_);

    for (size_t i = 0; i < NUM_AXIS_KEYS; ++i) {
        if (!axes.set[i] || !axes_[i].exists) continue;
        AxisRange& a = axes_[i];
        double v = axes.value[i];
        v = v != v ? 0.0 : clamp01(v);  // NaN -> 0
        LONG raw = a.lo + static_cast<LONG>(py_round(v * (a.hi - a.lo)));
        if (a.has_last && a.last == raw) continue;
        a.last = raw;
        a.has_last = true;
        api_->SetAxis(raw, rid, HID_USAGE[i]);
    }

    std::bitset<256> wanted;
    for (int i = 0; i < button_count; ++i)
        if (buttons[i] >= 1 && buttons[i] <= buttons_) wanted.set(buttons[i]);
    for (size_t b = 1; b < wanted.size(); ++b) {
        if (wanted[b] && !pressed_[b]) api_->SetBtn(1, rid, static_cast<UCHAR>(b));
        else if (!wanted[b] && pressed_[b]) api_->SetBtn(0, rid, static_cast<UCHAR>(b));
    }
    pressed_ = wanted;

    set_hat(hat);
}

void VJoyBackend::set_hat(int hat) {
    if (!api_ || hat == last_hat_) return;
    last_hat_ = hat;
    UINT rid = static_cast<UINT>(device_id_);
    if (disc_pov_ > 0) {
        // vJoy POV dyskretny: 0=gora, 1=prawo, 2=dol, 3=lewo, -1=srodek
        api_->SetDiscPov((hat >= 0 && hat <= 3) ? hat : -1, rid, 1);
    } else if (cont_pov_ > 0) {
        // ciagly: 0..35999 setne stopnia, -1 = srodek
        DWORD angle = (hat >= 0 && hat <= 3) ? static_cast<DWORD>(hat) * 9000 : 0xFFFFFFFF;
        api_->SetContPov(angle, rid, 1);
    }
}

std::string VJoyBackend::status_text() const {
    if (!open_) return "vJoy nieaktywne";
    std::string axes;
    for (const auto& k : available_axes()) axes += (axes.empty() ? "" : ", ") + k;
    if (axes.empty()) axes = "brak osi";
    return std::format("vJoy #{} - osie: {}; przyciski: {}; POV: {}; {}", device_id_, axes, buttons_,
                       disc_pov_ + cont_pov_, ffb_capable_ ? "FFB tak" : "FFB nie");
}

std::string VJoyBackend::version_text() const {
    if (!version_) return {};
    return std::format("{} (wersja {:x}.{:02x})", product_.empty() ? "vJoy" : product_, (version_ >> 8) & 0xFF,
                       version_ & 0xFF);
}

} // namespace wb

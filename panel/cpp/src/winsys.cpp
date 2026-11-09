#include "winsys.h"

#include <dwmapi.h>

#include <algorithm>
#include <charconv>
#include <cwchar>

namespace wb::winsys {

namespace {

HANDLE g_mutex = nullptr;  // otwarty do konca procesu, Windows zamknie go sam

bool cloaked(HWND hwnd) {
    DWORD value = 0;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &value, sizeof(value))) && value != 0;
}

// Widoczna ramka okna. GetWindowRect liczy tez niewidoczne brzegi do zmiany
// rozmiaru - pelnoekranowa gra nie "zakrywalaby" zmaksymalizowanego panelu.
std::optional<std::array<int, 4>> frame(HWND hwnd) {
    RECT r{};
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
        GetWindowRect(hwnd, &r))
        return std::array<int, 4>{r.left, r.top, r.right, r.bottom};
    return std::nullopt;
}

bool is_above(HWND top, HWND hwnd) {
    HWND h = GetWindow(hwnd, GW_HWNDPREV);
    for (int i = 0; i < 4000 && h; ++i) {  // zabezpieczenie przed petla
        if (h == top) return true;
        h = GetWindow(h, GW_HWNDPREV);
    }
    return false;
}

struct FindData {
    const std::wstring* prefix;
    DWORD own_pid;
    HWND found;
};

BOOL CALLBACK find_visit(HWND hwnd, LPARAM lparam) {
    auto* data = reinterpret_cast<FindData*>(lparam);
    wchar_t cls[64] = {};
    GetClassNameW(hwnd, cls, 64);
    // okno wersji C++ albo okno Tk wersji Python
    if (wcscmp(cls, WINDOW_CLASS) != 0 && wcscmp(cls, L"TkTopLevel") != 0) return TRUE;
    wchar_t text[256] = {};
    GetWindowTextW(hwnd, text, 256);
    if (std::wstring_view(text).substr(0, data->prefix->size()) != *data->prefix) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == data->own_pid) return TRUE;
    data->found = hwnd;
    return FALSE;  // mamy - koniec szukania
}

} // namespace

bool claim_single_instance(const wchar_t* name) {
    SetLastError(0);
    HANDLE handle = CreateMutexW(nullptr, FALSE, name);
    DWORD error = GetLastError();
    if (!handle) return true;  // lepiej uruchomic panel niz go zablokowac
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(handle);
        return false;
    }
    g_mutex = handle;
    return true;
}

bool bring_to_front(const std::wstring& title_prefix) {
    FindData data{&title_prefix, GetCurrentProcessId(), nullptr};
    EnumWindows(find_visit, reinterpret_cast<LPARAM>(&data));
    if (!data.found) return false;
    ShowWindow(data.found, IsIconic(data.found) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(data.found);
    return true;
}

std::wstring autostart_command(const std::filesystem::path& exe) {
    return L"\"" + std::filesystem::absolute(exe).wstring() + L"\" --minimized";
}

std::optional<std::wstring> autostart_target(const std::wstring& command) {
    auto a = command.find(L'"');
    if (a == std::wstring::npos) return std::nullopt;
    auto b = command.find(L'"', a + 1);
    if (b == std::wstring::npos) return std::nullopt;
    std::wstring first = command.substr(a + 1, b - a - 1);
    // Wpis wersji Python: "pythonw.exe" "skrypt.py" --minimized.
    auto c = command.find(L'"', b + 1);
    if (c != std::wstring::npos) {
        auto d = command.find(L'"', c + 1);
        if (d != std::wstring::npos) return command.substr(c + 1, d - c - 1);
    }
    return first;
}

std::optional<std::wstring> autostart_get(const wchar_t* key, const wchar_t* value) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return std::nullopt;
    wchar_t buf[2048];
    DWORD size = sizeof(buf), type = 0;
    LONG rc = RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<BYTE*>(buf), &size);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS) return std::nullopt;
    return std::wstring(buf, wcsnlen(buf, size / sizeof(wchar_t)));
}

bool autostart_set(const std::optional<std::wstring>& command, const wchar_t* key, const wchar_t* value) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    LONG rc;
    if (!command) {
        rc = RegDeleteValueW(k, value);
        if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
    } else {
        rc = RegSetValueExW(k, value, 0, REG_SZ, reinterpret_cast<const BYTE*>(command->c_str()),
                            static_cast<DWORD>((command->size() + 1) * sizeof(wchar_t)));
    }
    RegCloseKey(k);
    return rc == ERROR_SUCCESS;
}

std::optional<Geometry> parse_geometry(const std::string& raw) {
    auto a = raw.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::nullopt;
    std::string_view s(raw);
    s = s.substr(a, raw.find_last_not_of(" \t\r\n") - a + 1);

    const char* p = s.data();
    const char* end = p + s.size();
    int v[4];
    auto number = [&](int& out, bool sign) {
        if (p == end) return false;
        if (!sign && *p == '-') return false;
        auto [q, ec] = std::from_chars(p, end, out);
        if (ec != std::errc()) return false;
        p = q;
        return true;
    };
    if (!number(v[0], false) || p == end || *p++ != 'x') return std::nullopt;
    if (!number(v[1], false) || p == end || *p++ != '+') return std::nullopt;
    if (!number(v[2], true) || p == end || *p++ != '+') return std::nullopt;
    if (!number(v[3], true) || p != end) return std::nullopt;
    if (v[0] <= 0 || v[1] <= 0) return std::nullopt;
    return Geometry{v[0], v[1], v[2], v[3]};
}

std::optional<std::array<int, 4>> work_area_at(int x, int y, int w) {
    RECT strip{x, y, x + w, y + 30};
    HMONITOR monitor = MonitorFromRect(&strip, MONITOR_DEFAULTTONULL);
    if (!monitor) return std::nullopt;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return std::nullopt;
    const RECT& work = info.rcWork;
    long wide = std::min(strip.right, work.right) - std::max(strip.left, work.left);
    long tall = std::min(strip.bottom, work.bottom) - std::max(strip.top, work.top);
    if (wide < 120 || tall < 20) return std::nullopt;
    return std::array<int, 4>{work.left, work.top, work.right, work.bottom};
}

bool covers(const std::array<int, 4>& outer, const std::array<int, 4>& inner, int slack) {
    return outer[0] - slack <= inner[0] && outer[1] - slack <= inner[1] && outer[2] + slack >= inner[2] &&
           outer[3] + slack >= inner[3];
}

bool window_hidden(HWND hwnd) {
    if (!hwnd) return false;
    if (cloaked(hwnd)) return true;
    HWND fg = GetForegroundWindow();
    if (!fg || fg == hwnd) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;  // nasze menu albo okno dialogowe
    if (IsIconic(fg) || !IsWindowVisible(fg) || cloaked(fg) || !is_above(fg, hwnd)) return false;
    auto outer = frame(fg);
    auto inner = frame(hwnd);
    return outer && inner && covers(*outer, *inner);
}

bool is_foreground(HWND hwnd) { return hwnd && GetForegroundWindow() == hwnd; }

void flash_taskbar(HWND hwnd) {
    if (!hwnd) return;
    FLASHWINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = hwnd;
    info.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    FlashWindowEx(&info);
}

} // namespace wb::winsys

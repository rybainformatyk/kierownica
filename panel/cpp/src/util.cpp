#include "util.h"

#include <windows.h>

#include <vector>

namespace wb {

double monotonic() {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now() - std::chrono::seconds(1);
    return std::chrono::duration<double>(clock::now() - start).count();
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::filesystem::path exe_dir() {
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return std::filesystem::path(std::wstring(buf.data(), n)).parent_path();
}

std::string win_error_text(unsigned long code) {
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring msg;
    if (n && text) {
        msg.assign(text, n);
        LocalFree(text);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' '))
            msg.pop_back();
    }
    return narrow(msg) + " (kod " + std::to_string(code) + ")";
}

void Event::set() {
    {
        std::lock_guard lock(m_);
        flag_ = true;
    }
    cv_.notify_all();
}

void Event::clear() {
    std::lock_guard lock(m_);
    flag_ = false;
}

bool Event::is_set() const {
    std::lock_guard lock(m_);
    return flag_;
}

bool Event::wait(double seconds) const {
    std::unique_lock lock(m_);
    return cv_.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return flag_; });
}

void precise_sleep(double seconds) {
    if (seconds <= 0.0) return;
    // Zwykly Sleep czeka z dokladnoscia zegara systemu (15,6 ms, gdy gra
    // zaslania panel) - watek FFB robilby ~64 obiegi/s zamiast 400.
    thread_local HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        Sleep(static_cast<DWORD>(seconds * 1000.0 + 0.5));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(seconds * 1e7);
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
}

} // namespace wb

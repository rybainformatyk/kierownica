// Drobne narzedzia wspolne dla calej aplikacji.
#pragma once

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>

namespace wb {

// Zegar monotoniczny w sekundach. Zawsze > 0 - zero znaczy "jeszcze nie bylo".
double monotonic();

// Zaokraglanie jak round() w Pythonie: polowki do parzystej (FE_TONEAREST).
inline long long py_round(double v) { return std::llrint(v); }

inline double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
inline double clamp_sym(double v) { return v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v); }

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// Katalog z plikiem .exe - tam lezy config.json.
std::filesystem::path exe_dir();

// Opis bledu Windows po polsku (albo w jezyku systemu).
std::string win_error_text(unsigned long code);

// Odpowiednik threading.Event.
class Event {
public:
    void set();
    void clear();
    bool is_set() const;
    // Czeka najdluzej `seconds`; true, gdy zdarzenie jest ustawione.
    bool wait(double seconds) const;

private:
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    bool flag_ = false;
};

// Sleep z dokladnoscia ponizej milisekundy (zegar wysokiej rozdzielczosci).
void precise_sleep(double seconds);

} // namespace wb

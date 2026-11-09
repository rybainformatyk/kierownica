// Wspolny interfejs backendow wyjsciowych.
#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace wb {

// Klucze osi uzywane w calej aplikacji (identyczne z VJOY_AXIS_CHOICES
// poza "-", ktore oznacza brak przypisania).
inline constexpr int NUM_AXIS_KEYS = 9;
inline constexpr std::array<const char*, NUM_AXIS_KEYS> AXIS_KEYS = {"X",  "Y",   "Z",   "RX", "RY",
                                                                     "RZ", "SL0", "SL1", "WHL"};

inline int axis_index(std::string_view key) {
    for (int i = 0; i < NUM_AXIS_KEYS; ++i)
        if (key == AXIS_KEYS[static_cast<size_t>(i)]) return i;
    return -1;
}

// Wartosci osi 0..1; `set[i]` mowi, czy dana os w ogole ma byc ruszana.
struct AxisValues {
    std::array<double, NUM_AXIS_KEYS> value{};
    std::array<bool, NUM_AXIS_KEYS> set{};

    void put(std::string_view key, double v) {
        int i = axis_index(key);
        if (i < 0) return;
        value[static_cast<size_t>(i)] = v;
        set[static_cast<size_t>(i)] = true;
    }
};

// Backend nie moze dzialac - komunikat jest pokazywany uzytkownikowi.
class OutputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Urzadzenie wirtualne, ktore widza gry.
class OutputBackend {
public:
    virtual ~OutputBackend() = default;
    virtual void open() = 0;
    virtual void close() {}
    virtual bool is_open() const { return false; }
    // buttons: numery 1-based. hat: -1..3.
    virtual void update(const AxisValues& axes, const uint8_t* buttons, int button_count, int hat) = 0;
    virtual void center() {}
    virtual std::string status_text() const = 0;
};

// Tryb podgladu - nic nie wysyla, aplikacja dziala bez sterownika.
class NullBackend final : public OutputBackend {
public:
    void open() override { open_ = true; }
    void close() override { open_ = false; }
    bool is_open() const override { return open_; }
    void update(const AxisValues&, const uint8_t*, int, int) override {}
    std::string status_text() const override { return "Podgląd - gry nie widzą kierownicy"; }

private:
    bool open_ = false;
};

} // namespace wb

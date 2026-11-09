// Backend vJoy - wirtualna kierownica widziana przez gry jako urzadzenie
// DirectInput z osiami i Force Feedback.
//
// Wszystkie wywolania DLL idza przez pojedyncze funkcje SetAxis/SetBtn, a nie
// przez strukture JOYSTICK_POSITION - to eliminuje ryzyko rozjechania sie
// ukladu pol miedzy wersjami vJoy.
#pragma once

#include <windows.h>

#include <array>
#include <bitset>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base.h"

namespace wb {

inline constexpr int VJD_STAT_OWN = 0;   // urzadzenie nalezy do tej aplikacji
inline constexpr int VJD_STAT_FREE = 1;  // wolne
inline constexpr int VJD_STAT_BUSY = 2;  // zajete przez inna aplikacje
inline constexpr int VJD_STAT_MISS = 3;  // nie istnieje (nie wlaczone w vJoyConf)
inline constexpr int VJD_STAT_UNKN = 4;

const char* vjoy_status_text(int status);
const char* vjoy_axis_label(const std::string& key);

class VJoyNotAvailable : public OutputError {
public:
    using OutputError::OutputError;
};

using FfbHelper = DWORD(__cdecl*)(const void* packet, void* out);
using FfbGenCallback = void(WINAPI*)(void* data, void* user);

// Funkcje vJoyInterface.dll.
class VJoyApi {
public:
    explicit VJoyApi(const std::filesystem::path& dll_path);

    short(__cdecl* vJoyEnabled)() = nullptr;
    short(__cdecl* GetvJoyVersion)() = nullptr;
    void*(__cdecl* GetvJoyProductString)() = nullptr;
    int(__cdecl* GetVJDStatus)(UINT) = nullptr;
    int(__cdecl* AcquireVJD)(UINT) = nullptr;
    void(__cdecl* RelinquishVJD)(UINT) = nullptr;
    int(__cdecl* ResetVJD)(UINT) = nullptr;
    int(__cdecl* GetVJDButtonNumber)(UINT) = nullptr;
    int(__cdecl* GetVJDDiscPovNumber)(UINT) = nullptr;
    int(__cdecl* GetVJDContPovNumber)(UINT) = nullptr;
    int(__cdecl* GetVJDAxisExist)(UINT, UINT) = nullptr;
    int(__cdecl* GetVJDAxisMax)(UINT, UINT, LONG*) = nullptr;
    int(__cdecl* GetVJDAxisMin)(UINT, UINT, LONG*) = nullptr;
    int(__cdecl* SetAxis)(LONG, UINT, UINT) = nullptr;
    int(__cdecl* SetBtn)(int, UINT, UCHAR) = nullptr;
    int(__cdecl* SetDiscPov)(int, UINT, UCHAR) = nullptr;
    int(__cdecl* SetContPov)(DWORD, UINT, UCHAR) = nullptr;
    int(__cdecl* IsDeviceFfb)(UINT) = nullptr;

    // Funkcje FFB sa opcjonalne - starsze wersje DLL moga ich nie miec.
    void(__cdecl* FfbRegisterGenCB)(FfbGenCallback, void*) = nullptr;
    FfbHelper Ffb_h_DeviceID = nullptr;
    FfbHelper Ffb_h_Type = nullptr;
    FfbHelper Ffb_h_EBI = nullptr;
    FfbHelper Ffb_h_Eff_Report = nullptr;
    FfbHelper Ffb_h_EffOp = nullptr;
    FfbHelper Ffb_h_Eff_Constant = nullptr;
    FfbHelper Ffb_h_Eff_Ramp = nullptr;
    FfbHelper Ffb_h_Eff_Period = nullptr;
    FfbHelper Ffb_h_Eff_Cond = nullptr;
    FfbHelper Ffb_h_Eff_Envlp = nullptr;
    FfbHelper Ffb_h_DevCtrl = nullptr;
    FfbHelper Ffb_h_DevGain = nullptr;
};

std::optional<std::filesystem::path> find_vjoy_dll();
std::optional<std::filesystem::path> find_vjoy_config();

// Czy sterownik vJoy jest wlaczony. nullopt - vJoy nie jest zainstalowany.
std::optional<bool> vjoy_driver_enabled();

// Wlacza albo wylacza sterownik vJoy. Czeka na okno UAC i na vJoyConfig -
// wolac poza watkiem interfejsu.
std::pair<bool, std::string> set_vjoy_driver(bool enabled, double timeout_s = 20.0, HWND hwnd = nullptr);

struct VJoyDeviceInfo {
    int id = 0;
    int status = 0;
    std::string status_text;
    int buttons = 0;
    std::vector<std::string> axes;
    int disc_pov = 0;
    int cont_pov = 0;
    bool ffb = false;
};

struct VJoyDiagnostics {
    int bits = 64;
    std::optional<std::string> dll_path;
    bool dll_loaded = false;
    bool enabled = false;
    std::optional<int> version;
    std::optional<std::string> product;
    std::vector<VJoyDeviceInfo> devices;
    std::optional<std::string> error;
};

// Dane do zakladki diagnostyki - nigdy nie rzuca wyjatkiem.
VJoyDiagnostics vjoy_diagnostics();

// Jedno urzadzenie vJoy. Osie przyjmuja wartosci 0..1.
class VJoyBackend final : public OutputBackend {
public:
    explicit VJoyBackend(int device_id = 1) : device_id_(device_id) {}
    ~VJoyBackend() override { close(); }

    void open() override;
    void close() override;
    bool is_open() const override { return open_; }
    void update(const AxisValues& axes, const uint8_t* buttons, int button_count, int hat) override;
    void center() override { center(AxisValues{}); }
    // `neutral.set[i]` - wartosc spoczynkowa osi; bez wpisu srodek zakresu.
    void center(const AxisValues& neutral);
    std::string status_text() const override;
    std::string version_text() const;

    bool ffb_capable() const { return ffb_capable_; }
    VJoyApi* api() const { return api_; }
    int device_id() const { return device_id_; }
    int max_buttons() const { return buttons_; }
    std::vector<std::string> available_axes() const;

private:
    void set_hat(int hat);

    struct AxisRange {
        bool exists = false;
        LONG lo = 0;
        LONG hi = 32767;
        bool has_last = false;
        LONG last = 0;
    };

    int device_id_;
    VJoyApi* api_ = nullptr;
    bool open_ = false;
    std::array<AxisRange, NUM_AXIS_KEYS> axes_{};
    int buttons_ = 0;
    int disc_pov_ = 0;
    int cont_pov_ = 0;
    bool ffb_capable_ = false;
    std::bitset<256> pressed_;
    int last_hat_ = -2;
    int version_ = 0;
    std::string product_;
};

} // namespace wb

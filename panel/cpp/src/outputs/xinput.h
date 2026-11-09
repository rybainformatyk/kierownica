// Backend "Pad" - wirtualny kontroler Xbox 360 przez ViGEmBus.
//
// Czesc gier (NFS, wiele gier zrecznosciowych i portow z konsol) czyta
// wylacznie XInput i w ogole nie widzi urzadzen DirectInput. Ten backend
// pokazuje te sama kierownice jako zwyklego pada Xbox.
//
// Potrzebny jest sterownik ViGEmBus i biblioteka ViGEmClient.dll z katalogu
// vigem/ obok programu. Spusty Xboxa sa 8-bitowe, drazki 16-bitowe.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "base.h"

namespace wb {

// Maska przycisku XUSB (zgodna z XINPUT_GAMEPAD_*); 0, gdy nazwa nieznana.
unsigned xusb_mask(std::string_view name);
extern const std::map<std::string, std::string> PAD_BUTTON_LABELS;

// Gotowy raport pada. Wartosci juz w jednostkach XInputa.
struct PadState {
    unsigned buttons = 0;
    int lt = 0;
    int rt = 0;
    int lx = 0;
    int ly = 0;
    int rx = 0;
    int ry = 0;

    bool operator==(const PadState&) const = default;
    std::vector<std::string> pressed_names() const;
};

class ViGEmNotAvailable : public OutputError {
public:
    using OutputError::OutputError;
};

struct ViGEmDiagnostics {
    int bits = 64;
    std::optional<std::string> dll_path;
    bool dll_loaded = false;
    bool driver_ok = false;
    std::optional<std::string> error;
};

std::optional<std::filesystem::path> find_vigem_dll();
ViGEmDiagnostics vigem_diagnostics();

// Wirtualny pad Xbox 360. Gry widza go jako zwykly kontroler XInput.
class XInputBackend final : public OutputBackend {
public:
    ~XInputBackend() override { close(); }

    void open() override;
    void close() override;
    bool is_open() const override { return open_; }
    void update(const AxisValues&, const uint8_t*, int, int) override {}  // tryb pada uzywa send()
    void center() override { send(PadState{}, true); }
    // Wysyla raport. Powtorzenia sa pomijane, zeby nie meczyc sterownika.
    void send(const PadState& state, bool force = false);
    std::string status_text() const override;
    std::optional<int> slot() const { return slot_; }

private:
    void prime(std::optional<unsigned> slots_before);
    void nudge_then_zero();

    void* client_ = nullptr;
    void* target_ = nullptr;
    bool open_ = false;
    std::optional<PadState> last_;
    std::optional<int> slot_;  // gniazdo XInput, w ktorym Windows wyliczyl pada (0-3)
};

} // namespace wb

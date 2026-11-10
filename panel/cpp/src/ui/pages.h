// Zakladki panelu. Kazda buduje swoje drzewo widgetow i odswieza je co klatke.
#pragma once

#include <array>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../config.h"
#include "../engine.h"
#include "padview.h"
#include "widgets.h"

namespace wb::ui {

enum Tab { TAB_DASH, TAB_STEER, TAB_PEDALS, TAB_BUTTONS, TAB_G29, TAB_PAD, TAB_FFB, TAB_DIAG, TAB_COUNT };

struct ModeInfo {
    const char* key;
    const char* icon;
    const char* title;
    const char* desc;
};
extern const std::array<ModeInfo, 6> MODE_INFO;
extern const std::array<RGB, NUM_PEDALS> PEDAL_COLORS;
extern const std::array<int, 7> RANGE_PRESETS;

template <typename T, typename... A>
std::unique_ptr<T> mk(A&&... args) {
    return std::make_unique<T>(std::forward<A>(args)...);
}

std::string join(const std::vector<std::string>& parts, const char* sep);
std::string first_line(const std::string& s);
std::string hex_bytes(const uint8_t* data, size_t n);
int index_of(const Names& list, const std::string& value, int fallback = 0);
std::unique_ptr<Label> note(std::string text, float cap = 2000.0f);

// Dane jednej klatki - wspolne dla wszystkich zakladek.
struct Frame {
    EngineStatus st;
    LinkStats stats;
    LinkState link = LinkState::Disconnected;
    bool connected = false;
    bool live = false;
    std::optional<ProcessedState> state;
    double now = 0;
};

class PanelApp;

class Page {
public:
    explicit Page(PanelApp& app) : app_(app) {}
    virtual ~Page() = default;
    virtual std::unique_ptr<Widget> build() = 0;
    virtual void refresh(const Frame& f, bool visible) = 0;

protected:
    // Wiersze ustawien zwiazane z polem konfiguracji. Pola zyja w PanelApp,
    // wiec wskazniki sa wazne do konca programu; sync() dociaga wartosci, gdy
    // cos zmieni je z innego miejsca (normalize, profil, inna zakladka).
    Slider* bind_slider(Box* body, std::string title, std::string desc, double from, double to, double* field, Unit unit,
                        double resolution, std::optional<double> def, std::function<void()> after = nullptr);
    Slider* bind_slider_int(Box* body, std::string title, std::string desc, double from, double to, int* field, Unit unit,
                            double resolution, std::optional<double> def, std::function<void()> after = nullptr);
    Toggle* bind_toggle(Box* body, std::string title, std::string desc, bool* field, std::function<void()> after = nullptr);
    void add_row(Box* body, std::string title, std::string desc, WidgetPtr control, float control_w = 0.0f);
    void sync();

    PanelApp& app_;
    std::vector<std::function<void()>> syncs_;
};

class HomePage final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;

private:
    void refresh_status(const Frame& f);

    WheelDial* dial_ = nullptr;
    Label* angle_ = nullptr;
    Label* range_ = nullptr;
    Segmented* presets_ = nullptr;
    StatRow* counts_ = nullptr;
    StatRow* axis_ = nullptr;
    StatRow* velocity_ = nullptr;
    std::array<PedalBar*, NUM_PEDALS> bars_{};
    TraceChart* trace_ = nullptr;
    std::array<ModeTile*, 6> modes_{};
    Label* output_text_ = nullptr;
    StatusLine* s_board_ = nullptr;
    StatusLine* s_fw_ = nullptr;
    StatusLine* s_output_ = nullptr;
    StatusLine* s_ffb_ = nullptr;
    StatusLine* s_flags_ = nullptr;
    Label* hint_ = nullptr;
    ButtonGrid* grid_ = nullptr;
    HatIndicator* hat_ = nullptr;
    Label* out_ = nullptr;
    TorqueMeter* torque_ = nullptr;
};

class WheelPage final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;

private:
    void apply_cpr();
    void detect_cpr();

    Segmented* presets_ = nullptr;
    RangeRuler* ruler_ = nullptr;
    Slider* range_ = nullptr;
    Entry* cpr_ = nullptr;
    CurveEditor* curve_ = nullptr;
    StatRow* angle_ = nullptr;
    StatRow* axis_ = nullptr;
    Label* captured_ = nullptr;
    Combo* axis_combo_ = nullptr;
};

class PedalsPage final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;
    void select(int index);

    Slider* dz_low() { return dz_lo_; }
    Slider* gamma() { return gamma_; }
    int selected() const { return selected_; }

private:
    class Row;
    class Steps;
    PedalConfig& cur();
    void calib_start();
    void calib_apply_all();
    void calib_reset();

    int selected_ = 0;
    bool listening_ = false;
    int calib_step_ = 0;
    Steps* steps_ = nullptr;
    Label* calib_ = nullptr;
    Button* btn_listen_ = nullptr;
    Button* btn_save_ = nullptr;
    std::array<Row*, NUM_PEDALS> rows_{};
    Label* title_ = nullptr;
    Label* pct_ = nullptr;
    Combo* src_ = nullptr;
    Combo* axis_ = nullptr;
    Toggle* invert_ = nullptr;
    CurveEditor* curve_ = nullptr;
    RangeBar* range_ = nullptr;
    Slider* dz_lo_ = nullptr;
    Slider* dz_hi_ = nullptr;
    Slider* gamma_ = nullptr;
    Slider* smooth_ = nullptr;
};

class ButtonsPage final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;
    void select(int index);
    int selected() const { return selected_; }

private:
    class Tile;
    void set_hat(int choice);

    int selected_ = 0;
    uint32_t prev_mask_ = 0;
    std::array<Tile*, protocol::NUM_BUTTONS> tiles_{};
    Label* live_ = nullptr;
    Label* out_ = nullptr;
    Toggle* grab_ = nullptr;
    Label* title_ = nullptr;
    Label* pin_ = nullptr;
    Chip* state_ = nullptr;
    Stepper* target_ = nullptr;
    Toggle* nc_ = nullptr;
    Segmented* hat_choice_ = nullptr;
    HatIndicator* hat_ = nullptr;
    std::array<StatRow*, 4> hat_rows_{};
};

class G29Page final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;
    void stop();
    void escape();
    void copy_from_pad();
    PadView* view() { return view_; }

private:
    AppConfig& cfg();
    void sync_view();
    void notify(const std::string& text, double seconds = 4.0);
    void set_live(const std::optional<std::map<std::string, bool>>& active);
    void on_assign_request(const std::string& control);
    void on_clear(const std::string& control);
    void capture(uint32_t mask);
    void defaults();
    void clear_all();
    void wizard_start();
    void wizard_show();
    void wizard_advance(int delta);
    void wizard_clear();
    void wizard_stop(bool done = false);
    int filled();

    PadView* view_ = nullptr;
    Chip* state_ = nullptr;
    Label* live_ = nullptr;
    Box* tools_ = nullptr;
    Box* wizard_tools_ = nullptr;
    uint32_t prev_buttons_ = 0;
    double notice_until_ = 0;
    Names wizard_;
    int wizard_i_ = 0;
};

class PadPage final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;
    void stop_learning();
    void escape() { stop_learning(); }
    void wizard_start();
    bool wizard_active() const { return !wizard_.empty(); }
    void set_axis_source(const std::string& direction, const std::string& source);
    void defaults();
    void on_axis_click(const std::string& direction, float x, float y);
    PadView* view() { return view_; }

private:
    void notify(const std::string& text, double seconds = 4.0);
    bool source_available(const std::string& source) const;
    std::string axis_label(const std::string& direction) const;
    void sync_axis_labels();
    void on_assign_request(const std::string& control);
    void on_clear(const std::string& control);
    void clear_all();
    void capture(uint32_t mask);
    void wizard_show();
    void wizard_advance(int delta);
    void wizard_clear();
    void wizard_stop(bool done = false);
    std::vector<std::string> axis_warnings() const;
    void on_field_changed();
    void on_profile(int index);

    PadView* view_ = nullptr;
    Chip* state_ = nullptr;
    Label* live_ = nullptr;
    Label* conflict_ = nullptr;
    Box* tools_ = nullptr;
    Box* wizard_tools_ = nullptr;
    Combo* profile_ = nullptr;
    uint32_t prev_buttons_ = 0;
    Names wizard_;
    int wizard_i_ = 0;
    double notice_until_ = 0;
};

class FfbPage final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;

private:
    std::string state_text(const Frame& f) const;

    Label* title_ = nullptr;
    Toggle* on_ = nullptr;
    Label* state_ = nullptr;
    TorqueMeter* meter_ = nullptr;
    TraceChart* trace_ = nullptr;
    Label* effects_ = nullptr;
    Combo* direction_ = nullptr;
};

class DiagPage final : public Page {
public:
    using Page::Page;
    std::unique_ptr<Widget> build() override;
    void refresh(const Frame& f, bool visible) override;

    void update_vjoy_text();
    void append_log(const std::string& line, const std::string& level);
    std::string log_text() const;
    void sync_autostart();
    void set_flash_enabled(bool enabled);

private:
    void on_autostart(bool want);
    void copy_log();
    void refresh_g29(const Frame& f);

    std::map<std::string, StatTile*> tiles_;
    std::map<std::string, StatRow*> rows_;
    std::map<std::string, StatRow*> g29_rows_;
    Label* report_ = nullptr;
    double spark_t_ = 0;
    Button* btn_flash_ = nullptr;
    Toggle* autostart_ = nullptr;
    Label* autostart_other_ = nullptr;
    TextView* vjoy_ = nullptr;
    TextView* log_ = nullptr;
    Segmented* log_filter_ = nullptr;
    Label* config_ = nullptr;
    Combo* vjoy_id_ = nullptr;
};

} // namespace wb::ui

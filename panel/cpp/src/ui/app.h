// Okno glowne panelu kierownicy.
#pragma once

#include <windows.h>

#include <d2d1_1.h>

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../config.h"
#include "../engine.h"
#include "gfx.h"
#include "padview.h"
#include "widgets.h"

namespace wb::ui {

inline constexpr const char* APP_TITLE = "Panel kierownicy";
inline constexpr const char* APP_VERSION = "2.6";

struct AppOptions {
    bool minimized = false;
    std::filesystem::path selftest_dir;  // niepuste: bez okna, rysuje do plikow PNG
};

class G29Tab;

class PanelApp {
public:
    explicit PanelApp(const AppOptions& options);
    ~PanelApp();

    int run();
    int selftest();

    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    friend class G29Tab;

    enum { TAB_DASH, TAB_STEER, TAB_PEDALS, TAB_BUTTONS, TAB_G29, TAB_PAD, TAB_FFB, TAB_DIAG, TAB_COUNT };

    struct PedalWidgets {
        Combo* src = nullptr;
        Combo* axis = nullptr;
        CurvePlot* plot = nullptr;
        BarMeter* bar = nullptr;
        StatRow* range = nullptr;
        StatRow* seen = nullptr;
        Slider* dz_lo = nullptr;
        Slider* dz_hi = nullptr;
        Slider* gamma = nullptr;
        Slider* smooth = nullptr;
        Checkbox* invert = nullptr;
    };

    // --- budowa ---
    void build();
    std::unique_ptr<Widget> build_header();
    std::unique_ptr<Widget> build_dashboard();
    std::unique_ptr<Widget> build_steer_tab();
    std::unique_ptr<Widget> build_pedals_tab();
    std::unique_ptr<Card> build_pedal_card(const std::string& key);
    std::unique_ptr<Widget> build_buttons_tab();
    std::unique_ptr<Widget> build_pad_tab();
    std::unique_ptr<Widget> build_ffb_tab();
    std::unique_ptr<Widget> build_diag_tab();
    std::unique_ptr<Card> build_g29_card();
    std::unique_ptr<Widget> build_statusbar();
    std::unique_ptr<Combo> make_mode_combo(int width_chars);

    // --- okno ---
    bool create_window();
    void place_window();
    void create_target();
    void render();
    void render_to(ID2D1RenderTarget* rt);
    void layout_root();
    void request_paint();
    void remember_geometry();
    void store_ui_state();
    void on_close();
    void set_timer_period(UINT ms);
    void tick();
    Widget* widget_at(float x, float y);
    void update_hover(float x, float y);
    Pt to_dip(LPARAM lp) const;
    float scale() const { return static_cast<float>(dpi_) / 96.0f; }
    bool ask(const char* title, const std::string& text);
    void inform(const char* title, const std::string& text, UINT icon);
    void go_tab(int index);
    void on_tab_changed(int index);

    // --- polecenia ---
    void refresh_ports();
    std::string selected_port() const;
    void toggle_connect();
    void flash_firmware();
    void poll_flash();
    void sync_autostart();
    void on_autostart(bool want);
    void reset_board();
    void set_range(int degrees);
    void apply_cpr();
    void detect_cpr();
    void calib_start();
    void calib_apply_all();
    void set_calib_text(const std::string& text);
    void calib_reset();
    void sync_pedal_widgets();
    void on_button_map_change();
    void on_button_invert_change();
    void on_hat_change();
    void on_mode_change(int index);
    void sync_mode_widgets();
    void on_pad_field_changed();
    void sync_pad_profile_label();
    void on_pad_profile(int index);
    bool pad_source_available(const std::string& source) const;
    std::string pad_axis_label(const std::string& direction) const;
    void set_pad_axis_source(const std::string& direction, const std::string& source);
    void sync_pad_axis_labels();
    void on_pad_axis_click(const std::string& direction, float x, float y);
    void pad_notify(const std::string& text, double seconds = 4.0);
    void on_escape();
    void pad_stop_learning();
    void on_pad_assign_request(const std::string& control);
    void on_pad_clear(const std::string& control);
    void pad_defaults();
    void pad_clear_all();
    void pad_capture(uint32_t mask);
    void pad_wizard_start();
    void pad_wizard_show();
    void pad_wizard_advance(int delta);
    void pad_wizard_clear();
    void pad_wizard_stop(bool done = false);
    std::vector<std::string> pad_axis_warnings() const;
    void reopen_output(bool show_errors = false);
    void update_vjoy_text();
    void clear_log();
    void copy_log();
    std::string config_snapshot() const;
    void save_config();
    void refresh_saved_label();
    void enqueue_log(const std::string& level, const std::string& msg);
    void drain_log();

    // --- odswiezanie ---
    void refresh();
    void refresh_title(LinkState state, const std::string& port);
    void refresh_g29_card(bool connected, const EngineStatus& st);
    std::string g29_state(bool connected, const EngineStatus& st) const;
    std::pair<std::string, std::string> output_buttons_text(const ProcessedState& state) const;
    std::string output_hint(bool connected, bool vjoy_ok, bool pad_ok, const EngineStatus& st) const;
    std::string ffb_state_text(bool connected, const EngineStatus& st) const;
    void refresh_pad_tab(const std::optional<ProcessedState>& state, bool pad_ok, const EngineStatus& st);
    std::vector<std::string> axis_conflicts() const;

    bool selftest_checks(std::string& report);
    bool render_png(const std::filesystem::path& file);

    AppOptions options_;
    AppConfig cfg_;
    std::string saved_snapshot_;
    std::unique_ptr<WheelEngine> engine_;

    std::mutex log_mutex_;
    std::deque<std::pair<std::string, std::string>> log_queue_;

    // --- okno ---
    HWND hwnd_ = nullptr;
    UINT dpi_ = 96;
    ComPtr<ID2D1HwndRenderTarget> target_;
    UINT timer_period_ = 0;
    std::string title_;
    std::string normal_geometry_;
    bool zoomed_ = false;
    bool closing_ = false;
    bool hidden_ = false;
    double hidden_t_ = 0;
    bool was_connected_ = false;
    Widget* hover_ = nullptr;
    Widget* pressed_ = nullptr;

    // --- drzewo ---
    std::unique_ptr<Widget> header_;
    std::unique_ptr<TabBar> tabbar_;
    std::unique_ptr<Pages> pages_;
    std::unique_ptr<Widget> statusbar_;

    // --- naglowek ---
    StatusDot* dot_link_ = nullptr;
    StatusDot* dot_vjoy_ = nullptr;
    Label* lbl_link_ = nullptr;
    Label* lbl_vjoy_ = nullptr;
    Button* btn_connect_ = nullptr;
    Combo* port_combo_ = nullptr;
    std::vector<std::string> port_devices_;

    // --- pulpit ---
    WheelDial* dial_ = nullptr;
    StatRow* row_counts_ = nullptr;
    StatRow* row_axis_ = nullptr;
    StatRow* row_velocity_ = nullptr;
    std::array<std::pair<int, Button*>, 7> range_buttons_{};
    std::array<PedalGauge*, NUM_PEDALS> dash_bars_{};
    ButtonGrid* button_grid_ = nullptr;
    HatIndicator* hat_indicator_ = nullptr;
    Label* lbl_dash_out_ = nullptr;
    Label* lbl_output_ = nullptr;
    Label* lbl_output_hint_ = nullptr;
    TorqueMeter* torque_meter_ = nullptr;
    std::vector<Combo*> mode_combos_;

    // --- kierownica ---
    Slider* sl_range_ = nullptr;
    Entry* entry_cpr_ = nullptr;
    CurvePlot* steer_plot_ = nullptr;

    // --- pedaly ---
    Label* lbl_calib_ = nullptr;
    std::array<PedalWidgets, NUM_PEDALS> pedal_widgets_{};

    // --- przyciski ---
    Label* lbl_btn_live_ = nullptr;
    Label* lbl_btn_out_ = nullptr;
    std::vector<Combo*> button_map_;
    std::vector<Checkbox*> button_invert_;
    std::vector<Combo*> hat_combos_;
    std::vector<StatusDot*> btn_dots_;

    // --- pad ---
    PadView* pad_view_ = nullptr;
    Label* lbl_pad_state_ = nullptr;
    Label* lbl_pad_live_ = nullptr;
    Label* lbl_pad_conflict_ = nullptr;
    Box* pad_tools_ = nullptr;
    Box* pad_wizard_tools_ = nullptr;
    Slider* sl_pad_range_ = nullptr;
    Slider* sl_pad_dz_ = nullptr;
    Slider* sl_pad_anti_ = nullptr;
    Slider* sl_pad_lin_ = nullptr;
    Slider* sl_pad_trig_ = nullptr;
    Combo* pad_profile_ = nullptr;
    uint32_t pad_prev_buttons_ = 0;
    Names pad_wizard_;
    int pad_wizard_i_ = 0;
    double pad_notice_until_ = 0;

    // --- FFB ---
    Checkbox* chk_ffb_on_ = nullptr;
    Label* lbl_ffb_state_ = nullptr;
    TorqueMeter* ffb_meter_ = nullptr;
    Slider* sl_maxduty_ = nullptr;

    // --- diagnostyka ---
    std::map<std::string, StatRow*> diag_rows_;
    std::map<std::string, StatRow*> g29_rows_;
    Label* lbl_g29_report_ = nullptr;
    Sparkline* spark_fps_ = nullptr;
    Sparkline* spark_lat_ = nullptr;
    double spark_t_ = 0;
    Button* btn_flash_ = nullptr;
    Checkbox* chk_autostart_ = nullptr;
    Label* lbl_autostart_other_ = nullptr;
    TextView* txt_vjoy_ = nullptr;
    TextView* txt_log_ = nullptr;
    Label* lbl_log_copied_ = nullptr;
    double log_copied_until_ = 0;

    // --- pasek stanu ---
    Label* lbl_saved_ = nullptr;
    Label* lbl_status_ = nullptr;
    double saved_flash_until_ = 0;
    double dirty_check_t_ = 0;

    HANDLE flash_proc_ = nullptr;
    bool flash_reconnect_ = false;
    bool vjoy_was_switching_ = false;

    std::unique_ptr<G29Tab> g29tab_;
};

} // namespace wb::ui

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
#include <utility>
#include <vector>

#include "../config.h"
#include "../engine.h"
#include "gfx.h"
#include "pages.h"
#include "widgets.h"

namespace wb::ui {

inline constexpr const char* APP_TITLE = "Panel kierownicy";
inline constexpr const char* APP_VERSION = "3.0";

struct AppOptions {
    bool minimized = false;
    std::filesystem::path selftest_dir;  // niepuste: bez okna, rysuje do plikow PNG
};

class PanelApp {
public:
    explicit PanelApp(const AppOptions& options);
    ~PanelApp();

    int run();
    int selftest();

    LRESULT handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // --- dla zakladek ---
    AppConfig& cfg() { return cfg_; }
    const AppConfig& cfg() const { return cfg_; }
    WheelEngine& engine() { return *engine_; }
    const WheelEngine& engine() const { return *engine_; }
    void log(const std::string& level, const std::string& msg);
    void go_tab(int index);
    bool ask(const char* title, const std::string& text);
    void inform(const char* title, const std::string& text, UINT icon);

    std::string g29_state(bool connected, const EngineStatus& st) const;
    std::string output_hint(bool connected, const EngineStatus& st) const;
    std::pair<std::string, int> output_summary(bool connected, const EngineStatus& st) const;  // napis, poziom 0..2
    std::pair<std::string, std::string> output_buttons_text(const ProcessedState& state) const;
    std::vector<std::string> axis_conflicts() const;

    void set_mode(const std::string& mode);
    void set_range(int degrees);
    void reopen_output(bool show_errors = false);
    void toggle_connect();
    void reset_board();
    void flash_firmware();
    bool flashing() const { return flash_proc_ != nullptr; }
    bool firmware_available() const;
    void save_config();
    void open_mode_popover(const Rect& anchor);
    HWND window() const { return hwnd_; }

    G29Page* g29_page() { return g29_; }
    PadPage* pad_page() { return pad_; }

private:
    // --- budowa ---
    void build();
    void open_link_popover();

    // --- okno ---
    bool create_window();
    void place_window();
    void apply_frame_style();
    void create_target();
    void render();
    void render_to(ID2D1RenderTarget* rt);
    void layout_root();
    void request_paint();
    void remember_geometry();
    void store_ui_state();
    void on_close();
    void on_caption(int caption);
    void set_timer_period(UINT ms);
    void tick();
    Widget* widget_at(float x, float y);
    void update_hover(float x, float y);
    Pt to_dip(LPARAM lp) const;
    float scale() const { return static_cast<float>(dpi_) / 96.0f; }
    LRESULT hit_test_frame(HWND hwnd, LPARAM lp);

    void on_tab_changed(int index);
    void on_escape();
    void poll_flash();
    void refresh_ports();
    std::string selected_port() const;
    std::string config_snapshot() const;
    void refresh_save_button();
    void enqueue_log(const std::string& level, const std::string& msg);
    void drain_log();

    // --- odswiezanie ---
    void refresh();
    void refresh_title(LinkState state, const std::string& port);
    void refresh_shell(const Frame& f);

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
    double last_render_ = 0;
    double last_move_ = 0;
    bool was_connected_ = false;
    Widget* hover_ = nullptr;
    Widget* pressed_ = nullptr;

    // --- powloka ---
    std::unique_ptr<Sidebar> sidebar_;
    std::unique_ptr<TopBar> topbar_;
    std::unique_ptr<Pages> pages_;
    Chip* link_chip_ = nullptr;
    Button* btn_connect_ = nullptr;
    Chip* output_chip_ = nullptr;
    Button* btn_save_ = nullptr;
    bool dirty_ = false;
    double dirty_check_t_ = 0;
    int unread_alerts_ = 0;

    std::vector<std::string> port_labels_;
    std::vector<std::string> port_devices_;
    int port_index_ = 0;

    // --- zakladki ---
    std::array<std::unique_ptr<Page>, TAB_COUNT> page_list_{};
    PedalsPage* pedals_ = nullptr;
    ButtonsPage* buttons_ = nullptr;
    G29Page* g29_ = nullptr;
    PadPage* pad_ = nullptr;
    DiagPage* diag_ = nullptr;

    HANDLE flash_proc_ = nullptr;
    bool flash_reconnect_ = false;
    bool vjoy_was_switching_ = false;
};

} // namespace wb::ui

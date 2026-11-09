// Widok kontrolera Xbox w stylu x360ce i kierownicy G29.
//
//   * rysunek - pokazuje na zywo, co sie dzieje, i sluzy do klikania
//     przyciskow (klikniecie wlacza tryb nauki),
//   * lista przypisan obok - jeden wiersz na jedno gniazdo.
//
// Kazdy kierunek drazka jest OSOBNYM gniazdem, dokladnie jak w x360ce.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../config.h"
#include "widgets.h"

namespace wb::ui {

// Czesc rysunku: ksztalt, wyglad i przesuniecie (galka drazka).
struct Part {
    std::string name;
    Shape shape;
    CapStyle style;
    Glyph glyph;
    bool clickable = true;
    bool shadow = true;
    float dx = 0, dy = 0;
};

class PadCanvas : public Widget {
public:
    struct Handlers {
        std::function<void(const std::string&)> button_click;
        std::function<void(const std::string&)> button_clear;
        std::function<void(const std::string*)> hover;
        std::function<void(const std::string&, float, float)> axis_click;
        std::function<void(const std::string&)> axis_clear;
    };

    PadCanvas(int width, int height, Names controls, Names axis_parts, std::string id);
    void set_handlers(Handlers handlers) { handlers_ = std::move(handlers); }
    // Wirtualne wolanie w konstruktorze nie zadziala - tworca wola build().
    void build() { build_parts(); }

    void set_learning(const std::optional<std::string>& name);
    void set_pulse(bool on);
    void set_assigned(const std::map<std::string, bool>& assigned);
    void set_active(const std::map<std::string, bool>& active);
    void highlight(const std::optional<std::string>& part);
    void set_sticks(double lx, double ly, double rx, double ry);
    void set_triggers(int lt, int rt);

    // Do testow i klikania.
    std::vector<std::string> parts_at(float x, float y) const;
    std::string visual_state(const std::string& name) const { return state_of(name); }
    const std::vector<Part>& parts() const { return parts_; }

    Size measure(float) override { return {static_cast<float>(w_), static_cast<float>(h_)}; }
    void paint(Gfx& g) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    void mouse_rdown(float x, float y) override;
    bool hand_cursor() const override;

protected:
    virtual void paint_body(Paint& p);
    virtual void build_parts();
    virtual void paint_after_part(Gfx&, const Part&) {}
    virtual void paint_overlay(Gfx& g);

    Part& add(const std::string& name, Shape shape, const CapStyle& style, Glyph glyph = nullptr,
              bool clickable = true, bool shadow = true);
    std::string state_of(const std::string& name) const;
    const Sprite& sprite(const Part& part, const std::string& state);

    int w_, h_;
    Names controls_, axis_parts_;
    std::string id_;
    std::vector<Part> parts_;
    Handlers handlers_;

    std::map<std::string, bool> active_, assigned_;
    std::optional<std::string> learning_, highlight_, hover_;
    bool pulse_on_ = true;
    int lt_ = 0, rt_ = 0;
};

class WheelCanvas final : public PadCanvas {
public:
    WheelCanvas();
    void set_angle(double deg);
    void set_pedals(const std::array<double, NUM_PEDALS>& values);

protected:
    void paint_body(Paint& p) override;
    void build_parts() override;
    void paint_overlay(Gfx& g) override;

private:
    double angle_ = 0;
    std::array<double, NUM_PEDALS> pedals_{};
};

class XboxCanvas final : public PadCanvas {
public:
    XboxCanvas();

protected:
    void paint_after_part(Gfx& g, const Part& part) override;
};

// Opis listy gniazd.
struct SlotGroup {
    std::string title;
    bool axis;
    Names slots;
};

struct PadViewSpec {
    std::unique_ptr<PadCanvas> canvas;
    Names controls, axes;
    std::vector<std::vector<SlotGroup>> groups;
    LabelMap titles, row_labels;
    std::map<std::string, std::string> part_of_slot;
    std::map<std::string, Names> slots_of_part;
    std::map<std::string, Names> stick_slots;
    std::map<std::string, RGB> row_colors;
    std::string idle_hint;
    float name_minsize = 86;
};

// Rysunek + lista przypisan. Jedno gniazdo = jeden wiersz.
class PadView : public Widget {
public:
    struct Callbacks {
        std::function<void(const std::string&)> assign_request;
        std::function<void(const std::string&)> clear;
        std::function<void(const std::string&, float, float)> axis_click;
        std::function<void(const std::string&)> axis_clear;
        std::function<void()> cancel;
    };

    PadView(PadViewSpec spec, Callbacks callbacks);
    ~PadView() override;

    void start_learning(const std::string& name);
    void cancel_learning();
    const std::optional<std::string>& learning() const { return learning_; }
    void set_wizard(std::optional<int> step, std::optional<int> total);
    void set_held_buttons(const std::vector<int>& held);

    void set_assignments(const ButtonMap& assign);
    void set_axis_labels(const LabelMap& labels);
    void set_active(const std::map<std::string, bool>& active);
    void set_sticks(double lx, double ly, double rx, double ry);
    void set_triggers(int lt, int rt);
    void tick();  // miganie celu nauki i potwierdzenia wierszy

    PadCanvas* canvas() { return canvas_.get(); }

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    class Row;
    void on_row_click(const std::string& slot, float x, float y);
    void on_row_clear(const std::string& slot);
    void on_row_hover(const std::string* slot);
    void on_canvas_click(const std::string& name);
    void on_canvas_hover(const std::string* part);
    void on_canvas_axis(const std::string& part, float x, float y);
    void on_canvas_axis_clear(const std::string& part);
    void user_cancel();
    void apply_hover(const Names& slots, const std::optional<std::string>& part);
    void refresh_rows();
    std::string hint_text() const;
    float hint_height(float w);
    float list_min_height(float w);

    PadViewSpec spec_;
    Callbacks cb_;
    std::unique_ptr<PadCanvas> canvas_;
    std::vector<std::unique_ptr<Row>> rows_;
    std::map<std::string, Row*> row_of_;
    struct Header {
        std::string title;
        Rect r;
    };
    std::vector<Header> headers_;

    std::optional<std::string> learning_;
    std::optional<std::pair<int, int>> wizard_;
    std::vector<int> held_;
    bool pulse_on_ = true;
    double pulse_t_ = 0;
    Names hover_slots_;
    ButtonMap assign_;
    LabelMap axis_labels_;
    std::map<std::string, bool> active_;
    Rect hint_r_, legend_r_;
};

std::unique_ptr<PadView> make_pad_view(PadView::Callbacks callbacks);
std::unique_ptr<PadView> make_wheel_view(PadView::Callbacks callbacks);

// Kolejnosc kreatora "po kolei" w zakladce G29.
extern const Names G29_WIZARD_ORDER;

} // namespace wb::ui

// Widgety interfejsu - wszystko rysowane w jednym oknie (Direct2D).
//
// Drzewo: kazdy Widget zna swoj prostokat; kontenery (Box, Grid, Card, Scroll)
// rozkladaja dzieci. Napisy ustawia sie co klatke - setter porownuje wartosc
// i dopiero zmiana zamawia przerysowanie okna.
#pragma once

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "gfx.h"

namespace wb::ui {

struct Size {
    float w = 0, h = 0;
};

class Widget {
public:
    virtual ~Widget() = default;

    virtual Size measure(float avail_w) = 0;  // naturalny rozmiar; wysokosc dla szerokosci
    virtual void arrange(const Rect& rc) { r = rc; }
    virtual void paint(Gfx& g) = 0;
    virtual Widget* hit_test(float x, float y) { return (visible && r.contains(x, y)) ? this : nullptr; }

    virtual void mouse_enter() {}
    virtual void mouse_leave() {}
    virtual void mouse_move(float, float) {}
    virtual void mouse_down(float, float) {}
    virtual void mouse_up(float, float) {}
    virtual void mouse_rdown(float, float) {}
    virtual bool mouse_wheel(int) { return false; }  // true = obsluzone
    virtual bool key_down(unsigned) { return false; }
    virtual void char_input(wchar_t) {}
    virtual void focus_changed(bool) {}
    virtual bool hand_cursor() const { return false; }

    void set_visible(bool v);

    Rect r;
    bool visible = true;
};

using WidgetPtr = std::unique_ptr<Widget>;

// Globalny stan okna: przerysowanie, nakladki (listy, menu), fokus.
class Ui {
public:
    static Ui& get();

    void invalidate() { dirty = true; }
    void relayout() { layout_dirty = true; dirty = true; }

    void open_overlay(WidgetPtr overlay);
    void close_overlay();
    Widget* overlay() { return overlay_.get(); }
    void set_focus(Widget* w);
    Widget* focus() { return focus_; }

    // Odrzucone nakladki kasujemy dopiero poza obsluga zdarzenia.
    void collect_garbage() { trash_.clear(); }
    bool in_trash(const Widget* w) const {
        for (const auto& t : trash_)
            if (t.get() == w) return true;
        return false;
    }

    bool dirty = true;
    bool layout_dirty = true;
    Size window;
    Pt mouse;

private:
    WidgetPtr overlay_;
    std::vector<WidgetPtr> trash_;
    Widget* focus_ = nullptr;
};

inline void invalidate() { Ui::get().invalidate(); }
inline void relayout() { Ui::get().relayout(); }

// ------------------------------------------------------------- kontenery

struct Pack {
    float left = 0, top = 0, right = 0, bottom = 0;
    float flex = 0;                  // >0: dzieli wolne miejsce wzdluz osi
    enum Cross { Stretch, Start, Center, End } cross = Stretch;
};

class Box : public Widget {
public:
    explicit Box(bool horizontal, std::optional<RGB> background = std::nullopt)
        : horizontal_(horizontal), background_(background) {}

    template <typename T>
    T* add(std::unique_ptr<T> child, Pack pack = {}) {
        T* raw = child.get();
        items_.push_back({std::move(child), pack});
        relayout();
        return raw;
    }
    void add_spacer() { items_.push_back({std::make_unique<Spacer>(), Pack{.flex = 1}}); }
    bool empty() const { return items_.empty(); }

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    class Spacer : public Widget {
    public:
        Size measure(float) override { return {}; }
        void paint(Gfx&) override {}
    };
    struct Item {
        WidgetPtr w;
        Pack pack;
    };
    bool horizontal_;
    std::optional<RGB> background_;
    std::vector<Item> items_;
};

class Grid : public Widget {
public:
    // Kolumny: weight 0 = szerokosc tresci, >0 = dzieli reszte po rowno,
    // <0 = stala szerokosc.
    explicit Grid(std::vector<float> weights, std::vector<float> min_widths = {})
        : weights_(std::move(weights)), min_(std::move(min_widths)) {}

    template <typename T>
    T* place(std::unique_ptr<T> child, int row, int col, int colspan = 1, Pack pack = {}) {
        T* raw = child.get();
        cells_.push_back({std::move(child), row, col, colspan, pack});
        relayout();
        return raw;
    }

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    struct Cell {
        WidgetPtr w;
        int row, col, colspan;
        Pack pack;
    };
    std::vector<float> column_widths(float avail_w);
    std::vector<float> row_heights(const std::vector<float>& cols, float avail_w);

    std::vector<float> weights_;
    std::vector<float> min_;
    std::vector<Cell> cells_;
};

class Card : public Widget {
public:
    Card(std::string title, std::string subtitle = "", float wrap = 900);

    Box* body;
    Box* right_slot;

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    float header_h(float inner_w);
    std::string title_;
    std::unique_ptr<Box> body_;
    std::unique_ptr<Box> right_;
};

// Plaski pasek u gory zakladek Pad i G29.
class Strip : public Widget {
public:
    explicit Strip(std::string title);
    Box* row;
    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    std::string title_;
    std::unique_ptr<Box> row_;
};

class Scroll : public Widget {
public:
    static constexpr float BAR_W = 8;
    static constexpr float GUTTER = 14;

    explicit Scroll(WidgetPtr content) : content_(std::move(content)) {}
    Widget* content() { return content_.get(); }

    Size measure(float avail_w) override { return {avail_w, 0}; }
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;
    bool mouse_wheel(int delta) override;
    void mouse_down(float x, float y) override;
    void mouse_move(float x, float y) override;
    void mouse_up(float x, float y) override;

private:
    void set_offset(float offset);
    Rect thumb() const;

    WidgetPtr content_;
    float offset_ = 0;
    float content_h_ = 0;
    float wheel_acc_ = 0;
    bool dragging_ = false;
    float drag_y_ = 0;
    float drag_offset_ = 0;
};

class Pages : public Widget {
public:
    int add(WidgetPtr page);
    int current() const { return current_; }
    void select(int index);
    std::function<void(int)> on_changed;

    Size measure(float avail_w) override { return {avail_w, 0}; }
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    std::vector<WidgetPtr> pages_;
    int current_ = 0;
};

// -------------------------------------------------------------- kontrolki

class Label : public Widget {
public:
    Label(std::string text, Font font, RGB color, float cap = 0)
        : text_(std::move(text)), font_(font), color_(color), cap_(cap) {}

    void set(const std::string& text);
    void set_color(RGB color);
    void set(const std::string& text, RGB color) { set(text); set_color(color); }
    const std::string& text() const { return text_; }
    // Staly rozmiar - zmiana liczby nie przesuwa sasiadow.
    void set_fixed_width(float w) { fixed_w_ = w; }
    void set_align(Align a) { align_ = a; }

    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    std::string text_;
    Font font_;
    RGB color_;
    float cap_;              // >0: lamanie do tej szerokosci
    float fixed_w_ = 0;
    Align align_ = Align::Left;
};

class Button : public Widget {
public:
    enum Style { Normal, Accent, Small, Ghost };
    Button(std::string text, Style style, std::function<void()> command, float width = 0);

    void set(const std::string& text);
    void set_style(Style s);
    void set_enabled(bool e);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_down(float, float) override { pressed_ = true; invalidate(); }
    void mouse_up(float x, float y) override;
    bool hand_cursor() const override { return enabled_; }

private:
    std::string text_;
    Style style_;
    std::function<void()> command_;
    float width_;
    bool enabled_ = true, hover_ = false, pressed_ = false;
};

class Checkbox : public Widget {
public:
    Checkbox(std::string text, bool value, std::function<void(bool)> command, Font font = Font::Base);

    bool value() const { return value_; }
    void set(bool v);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_up(float x, float y) override;
    bool hand_cursor() const override { return true; }

private:
    std::string text_;
    bool value_;
    std::function<void(bool)> command_;
    Font font_;
    bool hover_ = false;
};

class Combo : public Widget {
public:
    Combo(std::vector<std::string> items, int index, std::function<void(int)> on_change, int width_chars);

    void set_items(std::vector<std::string> items, int index);
    void set_index(int index);
    int index() const { return index_; }
    const std::string& text() const;

    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_down(float x, float y) override;
    bool hand_cursor() const override { return true; }

private:
    std::vector<std::string> items_;
    int index_;
    std::function<void(int)> on_change_;
    int width_chars_;
    bool hover_ = false;
};

class Entry : public Widget {
public:
    Entry(std::string text, int width_chars) : text_(std::move(text)), width_chars_(width_chars) {}

    const std::string& text() const { return text_; }
    void set(const std::string& text);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_down(float x, float y) override;
    bool key_down(unsigned vk) override;
    void char_input(wchar_t c) override;
    void focus_changed(bool) override { invalidate(); }

private:
    std::string text_;
    int width_chars_;
    size_t caret_ = 0;
};

class StatusDot : public Widget {
public:
    StatusDot(float size, RGB background) : size_(size), bg_(background) {}
    void set(RGB color, bool glow = true);
    Size measure(float) override { return {size_, size_}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    float size_;
    RGB bg_;
    RGB color_ = MUTED;
    bool glow_ = true;
};

class StatRow : public Widget {
public:
    explicit StatRow(std::string label) : label_(std::move(label)) {}
    void set(const std::string& value);
    void set(const std::string& value, RGB color);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    std::string label_;
    std::string value_ = "-";
    RGB color_ = TEXT;
};

class Slider : public Widget {
public:
    using Format = std::function<std::string(double)>;
    Slider(std::string label, double from, double to, double value, Format fmt, double resolution,
           std::function<void(double)> command, std::string hint = "");

    double get() const;
    void set(double value);                // nie wola callbacku
    void apply(double value);              // jak ruch uzytkownika
    double value_at(float x) const;

    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_down(float x, float y) override;
    void mouse_move(float x, float y) override;
    void mouse_up(float, float) override;
    bool hand_cursor() const override { return true; }

private:
    double snap(double v) const;
    float x_of(double v) const;
    Rect track_rect() const;

    std::string label_, hint_;
    double from_, to_, value_, resolution_;
    Format fmt_;
    std::function<void(double)> command_;
    bool hover_ = false, dragging_ = false;
};

class BarMeter : public Widget {
public:
    BarMeter(std::string label, RGB color) : label_(std::move(label)), color_(color) {}
    void set(double value, std::optional<int> raw);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    std::string label_;
    RGB color_;
    double value_ = 0;
    std::optional<int> raw_;
};

class TorqueMeter : public Widget {
public:
    explicit TorqueMeter(float height) : height_(height) {}
    void set(double value);
    Size measure(float avail_w) override { return {avail_w, height_}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    float height_;
    double value_ = 0;
};

class PedalGauge : public Widget {
public:
    PedalGauge(std::string label, RGB color) : label_(std::move(label)), color_(color) {}
    void set(double value, std::optional<int> raw, bool enabled);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    std::string label_;
    RGB color_;
    double value_ = 0;
    std::optional<int> raw_;
    bool enabled_ = true;
};

class ButtonGrid : public Widget {
public:
    void set_mask(uint32_t mask);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    uint32_t mask_ = 0;
};

class HatIndicator : public Widget {
public:
    explicit HatIndicator(float size) : size_(size) {}
    void set(int direction);
    Size measure(float) override { return {size_, size_}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    float size_;
    int dir_ = -1;
};

class Sparkline : public Widget {
public:
    Sparkline(float height, RGB color) : height_(height), color_(color) {}
    void push(double value, double scale_max);
    Size measure(float avail_w) override { return {avail_w, height_}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    float height_;
    RGB color_;
    std::deque<double> data_;
    double top_ = 1;
    static constexpr size_t POINTS = 90;
};

class CurvePlot : public Widget {
public:
    CurvePlot(float height, RGB color, bool bipolar, std::array<std::string, 3> captions);
    void set_curve(const std::string& key, std::function<double(double)> fn,
                   std::vector<std::pair<double, double>> shade = {});
    void set_marker(std::optional<std::pair<double, double>> xy);
    void set_captions(const std::string& left, const std::string& middle, const std::string& right);

    Size measure(float avail_w) override { return {avail_w, height_ + 18}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    Pt to_px(double x, double y, float w) const;
    Sprite render(float w);

    float height_;
    RGB color_;
    bool bipolar_;
    std::array<std::string, 3> captions_;
    std::string key_;
    std::function<double(double)> fn_;
    std::vector<std::pair<double, double>> shade_;
    std::optional<std::pair<double, double>> marker_;
    Sprite sprite_;
    std::string drawn_key_;
    float drawn_w_ = 0;
    uint64_t drawn_gen_ = 0;
};

class WheelDial : public Widget {
public:
    explicit WheelDial(float size) : size_(size) {}
    void set_range(double degrees);
    void set_angle(double degrees);
    Size measure(float) override { return {size_, size_ + 62}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    Sprite wheel_sprite(RGB marker);
    float size_;
    double range_ = 900;
    double deg_ = 0;
};

class TextView : public Widget {
public:
    TextView(int rows, bool follow) : rows_(rows), follow_(follow) {}
    void clear();
    void set_lines(const std::vector<std::string>& lines, RGB color);
    void append(const std::string& line, RGB color);
    void set_rows(int rows);
    std::string joined() const;

    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override { return Widget::hit_test(x, y); }
    bool mouse_wheel(int delta) override;

private:
    struct Line {
        std::string text;
        RGB color;
    };
    float line_height();
    std::deque<Line> lines_;
    int rows_;
    bool follow_;
    float offset_ = 0;
    float content_h_ = 0;
};

class TabBar : public Widget {
public:
    TabBar(std::vector<std::string> labels, std::vector<std::string> icons, std::function<void(int)> command);
    void select(int index);

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    bool hand_cursor() const override { return hover_ >= 0; }

private:
    int item_at(float x, float y) const;
    std::vector<std::string> labels_, icons_;
    std::vector<Rect> rects_;
    std::function<void(int)> command_;
    int selected_ = 0, hover_ = -1;
};

// Pionowa kreska albo pozioma linia.
class Rule : public Widget {
public:
    Rule(RGB color, float w, float h) : color_(color), w_(w), h_(h) {}
    Size measure(float avail_w) override { return {w_ > 0 ? w_ : avail_w, h_}; }
    void paint(Gfx& g) override { g.fill_rect(r, color_); }
    Widget* hit_test(float, float) override { return nullptr; }

private:
    RGB color_;
    float w_, h_;
};

// Ciemne menu podreczne w kolorach panelu.
class PopupMenu {
public:
    explicit PopupMenu(std::string title) : title_(std::move(title)) {}
    void add(std::string label, std::function<void()> command, bool checked = false, std::string note = "");
    void add_separator();
    void popup(float x, float y);

private:
    struct Item {
        bool separator = false;
        std::string label, note;
        std::function<void()> command;
        bool checked = false;
    };
    friend class MenuOverlay;
    std::string title_;
    std::vector<Item> items_;
};

std::string upper_pl(const std::string& s);

inline constexpr const char* OFFLINE_LIVE = "Podgląd na żywo ruszy po połączeniu z płytką.";

} // namespace wb::ui

// Widgety interfejsu - wszystko rysowane w jednym oknie (Direct2D).
//
// Drzewo: kazdy Widget zna swoj prostokat; kontenery (Box, Grid, Columns, Card,
// Scroll) rozkladaja dzieci. Wartosci ustawia sie co klatke - setter porownuje
// je i dopiero zmiana zamawia przerysowanie okna.
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

// Ikony - czcionka Segoe Fluent Icons / MDL2 Assets (te same kody).
namespace ic {
inline constexpr const char* check = "";
inline constexpr const char* cancel = "";
inline constexpr const char* refresh = "";
inline constexpr const char* warn = "";
inline constexpr const char* info = "";
inline constexpr const char* undo = "";
inline constexpr const char* save = "";
inline constexpr const char* copy = "";
inline constexpr const char* trash = "";
inline constexpr const char* play = "";
inline constexpr const char* download = "";
inline constexpr const char* chevron_down = "";
inline constexpr const char* chevron_right = "";
inline constexpr const char* add = "";
inline constexpr const char* remove = "";
inline constexpr const char* usb = "";
inline constexpr const char* car = "";
inline constexpr const char* game = "";
inline constexpr const char* bolt = "";
inline constexpr const char* eye = "";
inline constexpr const char* link = "";
inline constexpr const char* target = "";
inline constexpr const char* power = "";
inline constexpr const char* min_window = "";
inline constexpr const char* max_window = "";
inline constexpr const char* restore_window = "";
inline constexpr const char* close_window = "";
} // namespace ic

struct Size {
    float w = 0, h = 0;
};

// Plynne przejscie wartosci - hover, przesuniecia, wskazniki.
struct Anim {
    float value = 0, target = 0;
    Anim() = default;
    explicit Anim(float v) : value(v), target(v) {}
    void set(float t) { target = t; }
    void snap(float t) { value = target = t; }
    bool step(float dt, float speed = 18.0f);  // true - jeszcze sie rusza
};

// Sprezyna z tlumieniem krytycznym - lagodny start i koniec, bez przeskoku.
// Do wartosci, ktorych zmiane ma byc widac jako ruch (zakres, wskaznik wyboru).
struct Spring {
    float value = 0, target = 0, velocity = 0;
    void set(float t) { target = t; }
    void snap(float t) { value = target = t; velocity = 0; }
    bool step(float dt, float omega = 13.0f, float rest = 0.05f);  // true - jeszcze sie rusza
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
    virtual void mouse_dbl(float x, float y) { mouse_down(x, y); }
    virtual bool mouse_wheel(int) { return false; }  // true = obsluzone
    virtual bool key_down(unsigned) { return false; }
    virtual void char_input(wchar_t) {}
    virtual void focus_changed(bool) {}
    virtual bool hand_cursor() const { return false; }
    virtual const std::string* tooltip() const { return nullptr; }

    void set_visible(bool v);

    Rect r;
    bool visible = true;
};

using WidgetPtr = std::unique_ptr<Widget>;

// Globalny stan okna: przerysowanie, nakladki, fokus, podpowiedzi, powiadomienia.
class Ui {
public:
    static Ui& get();

    void invalidate() { dirty = true; }
    void relayout() { layout_dirty = true; dirty = true; }

    // Nakladki (listy, menu, panele) leza stosem; klik obok zamyka wierzchnia.
    void open_overlay(WidgetPtr overlay);
    void close_overlay();
    void close_all_overlays();
    Widget* overlay();
    void arrange_overlays();
    void paint_overlays(Gfx& g);

    void set_focus(Widget* w);
    Widget* focus() { return focus_; }

    // Odrzucone nakladki kasujemy dopiero poza obsluga zdarzenia.
    void collect_garbage() { trash_.clear(); }
    bool in_trash(const Widget* w) const;

    void toast(std::string text, RGB color = GOOD, const char* icon = ic::check);
    void tick_toasts(double now);
    void paint_toasts(Gfx& g);

    void set_tooltip(const std::string* text);
    void paint_tooltip(Gfx& g);

    bool dirty = true;
    bool layout_dirty = true;
    Size window;
    Pt mouse;
    float dt = 1.0f;     // czas od poprzedniej klatki [s]
    double time = 0.0;   // czas biezacej klatki [s]

private:
    struct Toast {
        std::string text;
        RGB color;
        const char* icon;
        double born;
        Anim show;
    };
    std::vector<WidgetPtr> overlays_;
    std::vector<WidgetPtr> trash_;
    std::deque<Toast> toasts_;
    std::string tooltip_;
    Widget* focus_ = nullptr;
};

inline void invalidate() { Ui::get().invalidate(); }
inline void relayout() { Ui::get().relayout(); }
inline void animate(Anim& a, float speed = 18.0f) {
    if (a.step(Ui::get().dt, speed)) invalidate();
}

std::string upper_pl(const std::string& s);

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
        Widget* hit_test(float, float) override { return nullptr; }
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

// Kolumny, ktore w waskim oknie przechodza jedna pod druga. Wagi jak w Grid.
class Columns : public Widget {
public:
    Columns(std::vector<float> weights, float min_col, float gap = 16.0f)
        : weights_(std::move(weights)), min_col_(min_col), gap_(gap) {}

    template <typename T>
    T* add(std::unique_ptr<T> child) {
        T* raw = child.get();
        items_.push_back(std::move(child));
        relayout();
        return raw;
    }

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    int per_row(float w) const;
    std::vector<float> widths(size_t first, size_t count, float w) const;

    std::vector<float> weights_;
    float min_col_;
    float gap_;
    std::vector<WidgetPtr> items_;
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

// Wiersz ustawienia: po lewej nazwa i opis, po prawej kontrolka.
class SettingRow : public Widget {
public:
    SettingRow(std::string title, std::string desc, WidgetPtr control, float control_w = 0.0f, bool divider = true)
        : title_(std::move(title)), desc_(std::move(desc)), control_(std::move(control)), control_w_(control_w), divider_(divider) {}

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    bool stacked(float w) const;
    float control_width(float w);
    float text_width(float w);

    std::string title_, desc_;
    WidgetPtr control_;
    float control_w_;
    bool divider_;
};

class Scroll : public Widget {
public:
    static constexpr float BAR_W = 6;
    static constexpr float GUTTER = 14;

    Scroll(WidgetPtr content, float pad_x = 0, float pad_y = 0) : content_(std::move(content)), pad_x_(pad_x), pad_y_(pad_y) {}
    Widget* content() { return content_.get(); }

    Size measure(float avail_w) override { return {avail_w, 0}; }
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;
    bool mouse_wheel(int delta) override;
    void mouse_down(float x, float y) override;
    void mouse_move(float x, float y) override;
    void mouse_up(float x, float y) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }

private:
    void set_offset(float offset);
    Rect thumb() const;

    WidgetPtr content_;
    float pad_x_, pad_y_;
    float offset_ = 0;
    float content_h_ = 0;
    float wheel_acc_ = 0;
    bool dragging_ = false;
    bool hover_ = false;
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
    Anim show_{1.0f};
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
    enum Style { Normal, Accent, Small, Ghost, Danger };
    Button(std::string text, Style style, std::function<void()> command, float width = 0);

    void set(const std::string& text);
    void set_style(Style s);
    void set_enabled(bool e);
    void set_icon(const char* icon) { icon_ = icon; }
    void set_command(std::function<void()> command) { command_ = std::move(command); }
    void set_tooltip(std::string tip) { tip_ = std::move(tip); }
    bool enabled() const { return enabled_; }

    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_down(float, float) override { pressed_ = true; invalidate(); }
    void mouse_up(float x, float y) override;
    bool hand_cursor() const override { return enabled_; }
    const std::string* tooltip() const override { return tip_.empty() ? nullptr : &tip_; }

private:
    std::string text_;
    std::string tip_;
    Style style_;
    std::function<void()> command_;
    float width_;
    const char* icon_ = nullptr;
    bool enabled_ = true, hover_ = false, pressed_ = false;
    Anim hover_anim_;
};

class IconButton : public Widget {
public:
    IconButton(const char* icon, std::string tip, std::function<void()> command, float size = 34.0f)
        : icon_(icon), tip_(std::move(tip)), command_(std::move(command)), size_(size) {}

    Size measure(float) override { return {size_, size_}; }
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_up(float x, float y) override;
    bool hand_cursor() const override { return true; }
    const std::string* tooltip() const override { return &tip_; }

private:
    const char* icon_;
    std::string tip_;
    std::function<void()> command_;
    float size_;
    bool hover_ = false;
    Anim hover_anim_;
};

class Toggle : public Widget {
public:
    Toggle(bool value, std::function<void(bool)> command);

    bool value() const { return value_; }
    void set(bool v);
    Size measure(float) override { return {42, 24}; }
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_up(float x, float y) override;
    bool hand_cursor() const override { return true; }

private:
    bool value_;
    std::function<void(bool)> command_;
    bool hover_ = false;
    Anim pos_, hover_anim_;
};

class Segmented : public Widget {
public:
    Segmented(std::vector<std::string> items, int index, std::function<void(int)> on_change, float height = 34.0f);

    void set_index(int index);
    int index() const { return index_; }
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    bool hand_cursor() const override { return hover_ >= 0; }

private:
    int item_at(float x) const;
    float item_w() const;

    std::vector<std::string> items_;
    int index_;
    std::function<void(int)> on_change_;
    float height_;
    int hover_ = -1;
    Spring pos_;
    bool placed_ = false;
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
    Anim hover_anim_;
};

// Pole tekstowe. Wpis zatwierdza Enter, Tab albo utrata fokusu (klik obok,
// np. w przycisk "Zapisz") - jak w Slider; Esc wraca do tekstu sprzed edycji.
// Bez zatwierdzenia strona przy odswiezaniu nadpisalaby wpis wartoscia z configu.
class Entry : public Widget {
public:
    using CommitFn = std::function<void(const std::string&)>;
    Entry(std::string text, int width_chars, CommitFn on_commit = nullptr)
        : text_(std::move(text)), width_chars_(width_chars), on_commit_(std::move(on_commit)) {}

    const std::string& text() const { return text_; }
    void set(const std::string& text);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_down(float x, float y) override;
    bool key_down(unsigned vk) override;
    void char_input(wchar_t c) override;
    void focus_changed(bool focused) override;

private:
    std::string text_;
    std::string before_edit_;  // tekst w chwili dostania fokusu
    int width_chars_;
    size_t caret_ = 0;
    CommitFn on_commit_;
};

class Stepper : public Widget {
public:
    Stepper(int min_value, int max_value, int value, std::function<void(int)> command, std::string zero_text = "");

    int value() const { return value_; }
    void set(int v);
    Size measure(float) override { return {132, 34}; }
    void paint(Gfx& g) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    bool hand_cursor() const override { return hover_ != 0; }

private:
    int zone(float x) const;
    int min_, max_, value_;
    std::function<void(int)> command_;
    std::string zero_text_;
    int hover_ = 0;  // -1 minus, 1 plus
};

// Pigulka ze stanem; klikalna, gdy ma polecenie.
class Chip : public Widget {
public:
    Chip(std::string text, RGB color, std::function<void()> command = nullptr, bool chevron = false);

    void set(const std::string& text, RGB color);
    const std::string& text() const { return text_; }
    void set_spinner(bool on);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override { return command_ ? Widget::hit_test(x, y) : nullptr; }
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_up(float x, float y) override;
    bool hand_cursor() const override { return command_ != nullptr; }

private:
    std::string text_;
    RGB color_;
    std::function<void()> command_;
    bool chevron_;
    bool spinner_ = false;
    bool hover_ = false;
    Anim hover_anim_;
};

// Jak ma wygladac wartosc suwaka i jak ja wpisywac.
struct Unit {
    int decimals = 0;
    const char* suffix = "";
    double scale = 1.0;
};

class Slider : public Widget {
public:
    Slider(double from, double to, double value, Unit unit, double resolution, std::function<void(double)> command,
           std::optional<double> default_value = std::nullopt);

    double get() const;
    void set(double value);                // nie wola callbacku
    void apply(double value);              // jak ruch uzytkownika
    double value_at(float x) const;

    Size measure(float avail_w) override { return {avail_w, 30}; }
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_down(float x, float y) override;
    void mouse_move(float x, float y) override;
    void mouse_up(float, float) override;
    void mouse_dbl(float x, float y) override;
    bool key_down(unsigned vk) override;
    void char_input(wchar_t c) override;
    void focus_changed(bool focused) override;
    bool hand_cursor() const override { return true; }

private:
    double snap(double v) const;
    float x_of(double v) const;
    Rect track_rect() const;
    Rect bubble_rect() const;
    Rect reset_rect() const;
    bool changed() const;
    void commit_edit();
    std::string shown() const;

    double from_, to_, value_, resolution_;
    Unit unit_;
    std::function<void(double)> command_;
    std::optional<double> default_;
    bool hover_ = false, dragging_ = false, editing_ = false;
    std::string edit_;
    Anim grow_;
    Spring pos_;     // polozenie uchwytu 0..1
    bool placed_ = false;
};

// -------------------------------------------------------------- wskazniki

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
    const std::string& value() const { return value_; }
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    std::string label_;
    std::string value_ = "-";
    RGB color_ = TEXT;
};

// Kafelek z duza liczba i opcjonalnym wykresem historii.
class StatTile : public Widget {
public:
    StatTile(std::string label, bool spark = false);
    void set(const std::string& value, RGB color = TEXT);
    void push(double value, double scale_max);
    Size measure(float avail_w) override { return {avail_w, spark_ ? 112.0f : 76.0f}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    std::string label_;
    std::string value_ = "-";
    RGB color_ = TEXT;
    bool spark_;
    std::deque<double> data_;
    double top_ = 1;
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

class PedalBar : public Widget {
public:
    PedalBar(std::string label, RGB color, float width, float bar_h)
        : label_(std::move(label)), color_(color), width_(width), bar_h_(bar_h) {}
    void set(double value, std::optional<int> raw, bool enabled);
    Size measure(float) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    std::string label_;
    RGB color_;
    float width_, bar_h_;
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

// Wykres krzywej odpowiedzi - krawedzie martwych stref i sama krzywa mozna
// chwycic mysza. Wspolrzedne funkcji: 0..1, a w trybie bipolar -1..1.
class CurveEditor : public Widget {
public:
    struct Handlers {
        std::function<void(double)> low, high, gamma;
    };
    CurveEditor(float height, RGB color, bool bipolar, std::array<std::string, 3> captions, Handlers handlers = {});

    void set_curve(std::function<double(double)> fn);
    void set_params(double dz_low, double dz_high, double gamma);
    void set_marker(std::optional<std::pair<double, double>> xy);
    void set_captions(const std::string& left, const std::string& middle, const std::string& right);
    void set_color(RGB color);

    Size measure(float avail_w) override { return {avail_w, height_ + 20}; }
    void paint(Gfx& g) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    void mouse_up(float, float) override;
    bool hand_cursor() const override { return hover_ != None; }

private:
    enum Part { None, Low, High, Curve };
    Part part_at(float x, float y) const;
    Pt to_px(double x, double y) const;
    void drag_to(float x, float y);

    float height_;
    RGB color_;
    bool bipolar_;
    std::array<std::string, 3> captions_;
    Handlers h_;
    std::function<double(double)> fn_;
    double dz_low_ = 0, dz_high_ = 0, gamma_ = 1;
    std::optional<std::pair<double, double>> marker_;
    Part hover_ = None, drag_ = None;
};

// Surowy zakres ADC: co skalibrowane, co widziano i gdzie jest pedal.
class RangeBar : public Widget {
public:
    explicit RangeBar(RGB color) : color_(color) {}
    void set(int cal_min, int cal_max, std::optional<int> raw, std::optional<std::pair<int, int>> seen);
    void set_color(RGB color);
    Size measure(float avail_w) override { return {avail_w, 50}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    RGB color_;
    int cal_min_ = 0, cal_max_ = 4095;
    std::optional<int> raw_;
    std::optional<std::pair<int, int>> seen_;
};

class WheelDial : public Widget {
public:
    explicit WheelDial(float size) : size_(size) {}
    void set_range(double degrees);
    void set_angle(double degrees);
    // Zakres w trakcie animacji - do napisow obok tarczy.
    double shown_range() const { return placed_ ? shown_.value : range_; }
    Size measure(float) override { return {size_, size_}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    Sprite wheel_sprite(RGB marker);
    Sprite light_sprite(RGB marker);
    float size_;
    double range_ = 900;
    double deg_ = 0;
    Spring shown_;   // zakres, ktory widac - dogania range_
    Anim flash_;     // blysk skali po zmianie zakresu
    bool placed_ = false;
};

// Linijka zakresu: podzialka w stopniach rozciaga sie i sciska razem z
// zakresem, a znacznik pokazuje, gdzie jest kierownica.
class RangeRuler : public Widget {
public:
    void set_range(double degrees);
    void set_angle(std::optional<double> degrees);
    Size measure(float avail_w) override { return {avail_w, 52}; }
    void paint(Gfx& g) override;
    Widget* hit_test(float, float) override { return nullptr; }

private:
    double range_ = 900;
    std::optional<double> deg_;
    Spring shown_;
    Anim flash_;
    bool placed_ = false;
};

// Telemetria na zywo: ostatnie sekundy jazdy jako przesuwane wykresy.
class TraceChart : public Widget {
public:
    struct Series {
        std::string name;
        RGB color;
        int lane;
        bool area;
    };
    struct Lane {
        std::string title;
        bool bipolar;
        float weight;
    };
    static constexpr size_t MAX_SERIES = 6;

    TraceChart(std::vector<Series> series, std::vector<Lane> lanes, float height, double window_s = 10.0);
    void push(double now, const std::array<float, MAX_SERIES>& values, bool repaint);
    void set_live(bool live);

    Size measure(float avail_w) override { return {avail_w, height_}; }
    void paint(Gfx& g) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    bool hand_cursor() const override { return hover_ >= 0; }

private:
    struct Sample {
        double t;
        std::array<float, MAX_SERIES> v;
    };
    int legend_at(float x, float y) const;
    float legend_x(size_t index) const;

    std::vector<Series> series_;
    std::vector<Lane> lanes_;
    float height_;
    double window_;
    std::deque<Sample> samples_;
    std::array<bool, MAX_SERIES> shown_;
    double now_ = 0;
    bool live_ = false;
    int hover_ = -1;
};

class TextView : public Widget {
public:
    TextView(int rows, bool follow) : rows_(rows), follow_(follow) {}
    void clear();
    void set_lines(const std::vector<std::string>& lines, RGB color);
    void append(const std::string& line, RGB color, int level = 0);
    void set_rows(int rows);
    void set_filter(int min_level);   // 0 wszystko, 1 ostrzezenia i bledy, 2 bledy
    std::string joined() const;

    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    bool mouse_wheel(int delta) override;

private:
    struct Line {
        std::string text;
        RGB color;
        int level;
    };
    float line_height();
    std::deque<Line> lines_;
    int rows_;
    bool follow_;
    int min_level_ = 0;
    float offset_ = 0;
    float content_h_ = 0;
};

// Duzy kafelek wyboru (tryb wyjscia).
class ModeTile : public Widget {
public:
    ModeTile(const char* icon, std::string title, std::string desc, std::function<void()> command);
    void set_selected(bool selected);
    Size measure(float avail_w) override;
    void paint(Gfx& g) override;
    void mouse_enter() override { hover_ = true; invalidate(); }
    void mouse_leave() override { hover_ = false; invalidate(); }
    void mouse_up(float x, float y) override;
    bool hand_cursor() const override { return true; }

private:
    const char* icon_;
    std::string title_, desc_;
    std::function<void()> command_;
    bool selected_ = false, hover_ = false;
    Anim sel_anim_, hover_anim_;
};

enum class Level { Off, Ok, Warn, Bad, Busy };

// Wiersz listy stanu: ikona, opis i - gdy jest co naprawic - przycisk.
class StatusLine : public Widget {
public:
    StatusLine();
    void set(Level level, const std::string& title, const std::string& detail);
    void set_action(const std::string& text, std::function<void()> command);
    Level level() const { return level_; }
    const std::string& title() const { return title_; }

    Size measure(float avail_w) override;
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;

private:
    float action_w();
    Level level_ = Level::Off;
    std::string title_, detail_;
    std::unique_ptr<Button> action_;
};

// ------------------------------------------------------------------ powloka

class Sidebar : public Widget {
public:
    struct Item {
        std::string label, icon, group;
    };
    static constexpr float WIDE = 232, COMPACT = 76;

    Sidebar(std::vector<Item> items, std::function<void(int)> command);
    void select(int index);
    void set_badge(int index, const std::string& text, RGB color);
    void set_compact(bool compact);
    bool compact() const { return compact_; }
    float width() const { return compact_ ? COMPACT : WIDE; }
    void set_cluster(bool live, double deg, double range, double throttle, double brake, double clutch);
    bool is_drag_area(float x, float y) const;

    Size measure(float) override { return {width(), 0}; }
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    bool hand_cursor() const override { return hover_ >= 0; }
    const std::string* tooltip() const override;

private:
    int item_at(float x, float y) const;
    void paint_cluster(Gfx& g);

    std::vector<Item> items_;
    std::vector<Rect> rects_;
    std::vector<std::pair<float, std::string>> headers_;
    std::vector<std::pair<std::string, RGB>> badges_;
    std::function<void(int)> command_;
    int selected_ = 0, hover_ = -1;
    bool compact_ = false;
    Anim indicator_;
    bool indicator_placed_ = false;
    std::vector<Anim> hover_anim_;
    Rect cluster_;
    bool show_cluster_ = true;
    bool live_ = false;
    double deg_ = 0, range_ = 900, throttle_ = 0, brake_ = 0, clutch_ = 0;
    Spring range_shown_;
    bool range_placed_ = false;
};

class TopBar : public Widget {
public:
    static constexpr float HEIGHT = 76;
    explicit TopBar(std::function<void(int)> on_caption);

    Box* actions;
    void set_title(const std::string& title, const std::string& subtitle);
    void set_maximized(bool maximized);
    int caption_at(float x, float y) const;   // 0 brak, 1 minimalizuj, 2 maksymalizuj, 3 zamknij
    void set_caption_hover(int caption);
    void press_caption(int caption);

    Size measure(float avail_w) override { return {avail_w, HEIGHT}; }
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;
    void mouse_move(float x, float y) override;
    void mouse_leave() override;
    void mouse_down(float x, float y) override;
    void mouse_up(float x, float y) override;

private:
    Rect caption_rect(int caption) const;
    std::string title_, subtitle_;
    std::unique_ptr<Box> actions_;
    std::function<void(int)> on_caption_;
    bool maximized_ = false;
    int hover_ = 0, pressed_ = 0;
};

// ----------------------------------------------------------------- nakladki

// Panel zakotwiczony pod elementem - np. wybor portu albo trybu.
class Popover : public Widget {
public:
    Popover(WidgetPtr content, const Rect& anchor, float width);

    Size measure(float) override { return Ui::get().window; }
    void arrange(const Rect& rc) override;
    void paint(Gfx& g) override;
    Widget* hit_test(float x, float y) override;
    void mouse_down(float x, float y) override;
    void mouse_rdown(float x, float y) override;
    bool key_down(unsigned vk) override;

private:
    WidgetPtr content_;
    Rect anchor_, box_;
    float width_;
    Anim show_;
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

inline constexpr const char* OFFLINE_LIVE = "Podgląd na żywo ruszy po połączeniu z płytką.";

} // namespace wb::ui

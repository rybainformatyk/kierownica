// Grafika interfejsu - Direct2D + DirectWrite.
//
// Gfx rysuje na biezacym celu (okno albo bitmapa w --selftest). Paint to
// odpowiednik art.Paint z wersji Python: maluje gotowy obrazek (kontrolka,
// korpus pada) raz, a potem na ekranie jest juz tylko bitmapa.
//
// Wspolrzedne sa w DIP (1/96 cala), wiec uklad nie zalezy od skalowania.
#pragma once

#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wb::ui {

using Microsoft::WRL::ComPtr;
using RGB = uint32_t;  // 0xRRGGBB

// ---------------------------------------------------------------- paleta
//
// Grafit z cienkimi liniami, jeden akcent. Kolory telemetrii (gaz, hamulec,
// sprzeglo, moment) pojawiaja sie tylko tam, gdzie cos znacza.

inline constexpr RGB BG = 0x0a0c11;         // tlo okna
inline constexpr RGB SIDEBAR = 0x0d1016;    // pasek nawigacji
inline constexpr RGB PANEL = 0x131720;      // karta
inline constexpr RGB PANEL_HI = 0x1b212b;   // pole w karcie
inline constexpr RGB ELEV = 0x262e3a;       // element pod kursorem
inline constexpr RGB BORDER = 0x232a35;     // linia wlosowa
inline constexpr RGB BORDER_HI = 0x364152;  // linia pod kursorem
inline constexpr RGB TRACK = 0x222a36;      // rowek paska, nieaktywna dioda
inline constexpr RGB TEXT = 0xf0f4fa;
inline constexpr RGB TEXT_DIM = 0xa9b4c4;
inline constexpr RGB MUTED = 0x7f8b9e;
inline constexpr RGB ACCENT = 0x4cc2ff;
inline constexpr RGB ACCENT_HI = 0x83d6ff;
inline constexpr RGB ACCENT_DIM = 0x1b5478;
inline constexpr RGB ACCENT_DEEP = 0x123a55;
inline constexpr RGB ACCENT_INK = 0x06131d;  // tekst na tle akcentu
inline constexpr RGB GOOD = 0x35d399;
inline constexpr RGB WARN = 0xfbbf24;
inline constexpr RGB BAD = 0xfb7185;
inline constexpr RGB VIOLET = 0xa78bfa;
inline constexpr RGB WHITE = 0xffffff;
inline constexpr RGB BLACK = 0x000000;

RGB mix(RGB a, RGB b, double t);
inline RGB lighten(RGB c, double t) { return mix(c, WHITE, t); }
inline RGB darken(RGB c, double t) { return mix(c, BLACK, t); }

enum class Font {
    Body, BodyStrong, Small, SmallStrong, Tiny, TinyStrong, Caps,
    Head, Title, Num, NumLarge, NumHuge, NumGiant,
    Mono, MonoSmall, MonoStrong, Icon, IconSmall, IconLarge, Micro,
    Count,
    // nazwy uzywane przez rysunki pada i kierownicy
    Base = Body, Bold = BodyStrong, SmallBold = SmallStrong, TinyBold = TinyStrong, Heading = Head,
};

enum class Align { Left, Center, Right };

struct Pt {
    float x = 0, y = 0;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool contains(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
    bool intersects(const Rect& o) const {
        return x < o.right() && o.x < right() && y < o.bottom() && o.y < bottom();
    }
    Rect inset(float d) const { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
    Rect inset(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
};

// Ksztalty we wspolrzednych plotna, jak krotki w art.py. Katy w stopniach,
// 0 = w prawo, rosna zgodnie ze wskazowkami.
struct Shape {
    enum Kind { Circle, RRect, Poly, Ring } kind = Circle;
    float a = 0, b = 0, c = 0, d = 0, e = 0, f = 0;
    std::vector<Pt> pts;

    static Shape circle(float cx, float cy, float r);
    static Shape rrect(float x1, float y1, float x2, float y2, float r);
    static Shape poly(std::vector<Pt> points);
    static Shape ring(float cx, float cy, float ro, float ri, float a0, float a1);

    void bbox(float& x1, float& y1, float& x2, float& y2) const;
    Shape grow(float d) const;      // powiekszony o d (ujemne - pomniejszony)
    bool contains(float x, float y) const;
};

std::vector<Pt> ring_points(float cx, float cy, float ro, float ri, float a0, float a1, int steps = 48);

// Gotowy obrazek i polozenie jego lewego gornego rogu na plotnie.
struct Sprite {
    ComPtr<ID2D1Bitmap> bitmap;
    float x = 0, y = 0, w = 0, h = 0;
    explicit operator bool() const { return bitmap != nullptr; }
};

class Paint;

class Gfx {
public:
    static Gfx& get();

    bool init();                       // fabryki, czcionki
    ID2D1Factory1* factory() { return factory_.Get(); }
    IDWriteFactory* dwrite() { return dwrite_.Get(); }
    bool has_icon_font() const { return !icon_family_.empty(); }
    const std::wstring& display_family() const { return display_family_; }

    void set_target(ID2D1RenderTarget* rt);
    ID2D1RenderTarget* target() { return rt_; }
    // Utrata urzadzenia graficznego: obrazki trzeba zrobic od nowa.
    void drop_resources();
    uint64_t generation() const { return generation_; }

    // --- proste rysowanie na biezacym celu ---
    void clear(RGB color);
    void fill_rect(const Rect& r, RGB color, float alpha = 1.0f);
    void fill_rrect(const Rect& r, float radius, RGB color, float alpha = 1.0f);
    void stroke_rrect(const Rect& r, float radius, RGB color, float width = 1.0f, float alpha = 1.0f);
    void fill_circle(float cx, float cy, float r, RGB color, float alpha = 1.0f);
    void stroke_circle(float cx, float cy, float r, RGB color, float width = 1.0f, float alpha = 1.0f);
    void line(float x1, float y1, float x2, float y2, RGB color, float width = 1.0f, float alpha = 1.0f);
    void polyline(const std::vector<Pt>& pts, RGB color, float width, float alpha = 1.0f);
    void fill_poly(const std::vector<Pt>& pts, RGB color, float alpha = 1.0f);
    void arc(float cx, float cy, float r, float a0, float a1, RGB color, float width, float alpha = 1.0f);
    void fill_shape(const Shape& s, RGB color, float alpha = 1.0f);

    // Gradienty: pionowy miedzy gora i dolem prostokata, poziomy miedzy bokami.
    void fill_rrect_v(const Rect& r, float radius, RGB top, RGB bottom, float a_top = 1.0f, float a_bottom = 1.0f);
    void fill_rrect_h(const Rect& r, float radius, RGB left, RGB right, float a_left = 1.0f, float a_right = 1.0f);
    void fill_poly_v(const std::vector<Pt>& pts, float y0, float y1, RGB top, RGB bottom, float a_top, float a_bottom);
    void stroke_rrect_v(const Rect& r, float radius, RGB top, RGB bottom, float a_top, float a_bottom, float width = 1.0f);
    void fill_radial(const Rect& clip, float radius, float cx, float cy, float r, RGB inner, RGB outer, float a_inner, float a_outer);

    // Miekki cien / poswiata wokol prostokata (9 wycinkow z rozmytego wzoru).
    void shadow(const Rect& r, float radius, float blur, RGB color, float alpha, float dy = 0.0f);

    // Tlo tresci: ledwo widoczna poswiata z ditheringiem, bez widocznych pasow.
    void backdrop(float x, float y);

    void draw_sprite(const Sprite& s, float dx = 0, float dy = 0);
    void draw_sprite_rotated(const Sprite& s, float cx, float cy, float deg);

    void text(const Rect& r, std::string_view s, Font font, RGB color, Align align = Align::Left,
              bool wrap = false, float alpha = 1.0f);
    // Jak text(), ale za dlugi napis konczy sie wielokropkiem zamiast urwana litera.
    void text_fit(const Rect& r, std::string_view s, Font font, RGB color, Align align = Align::Left, float alpha = 1.0f);
    // Napis wysrodkowany w punkcie.
    void text_at(float x, float y, std::string_view s, Font font, RGB color, float alpha = 1.0f);
    // Rozmiar napisu; max_w <= 0 - bez lamania.
    D2D1_SIZE_F measure(std::string_view s, Font font, float max_w = 0.0f);
    // Kreciolek postepu; `time` w sekundach.
    void spinner(float cx, float cy, float r, RGB color, double time, float width = 2.0f);

    void push_clip(const Rect& r);
    void pop_clip();
    void push_rounded_clip(const Rect& r, float radius);
    void pop_rounded_clip();
    void begin_opacity(float alpha);
    void end_opacity();
    void push_translate(float dx, float dy);
    void pop_translate();

    // Obrazek z pamieci - render() wolany tylko przy pierwszym uzyciu.
    const Sprite& cached(const std::string& key, const std::function<Sprite()>& render);

    ComPtr<ID2D1SolidColorBrush> brush(RGB color, float alpha = 1.0f);
    ComPtr<ID2D1Geometry> geometry(const Shape& s);
    ComPtr<ID2D1StrokeStyle> round_stroke();
    ComPtr<ID2D1StrokeStyle> flat_stroke();
    IDWriteTextFormat* format(Font font);
    ComPtr<IDWriteTextLayout> layout(std::string_view s, Font font, float max_w, Align align, bool wrap, bool trim = false);

private:
    Gfx() = default;
    ComPtr<ID2D1LinearGradientBrush> linear(RGB a, RGB b, float alpha_a, float alpha_b, D2D1_POINT_2F p0, D2D1_POINT_2F p1);

    ComPtr<ID2D1Factory1> factory_;
    ComPtr<IDWriteFactory> dwrite_;
    ID2D1RenderTarget* rt_ = nullptr;
    uint64_t generation_ = 1;
    ComPtr<ID2D1SolidColorBrush> solid_;
    ComPtr<ID2D1StrokeStyle> round_;
    ComPtr<ID2D1StrokeStyle> flat_;
    ComPtr<IDWriteTypography> tabular_;
    std::wstring icon_family_;
    std::wstring display_family_;
    std::vector<ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<std::string, ComPtr<IDWriteTextLayout>> layouts_;
    std::unordered_map<std::string, Sprite> sprites_;
    std::unordered_map<std::string, ComPtr<ID2D1LinearGradientBrush>> gradients_;
    std::vector<D2D1_MATRIX_3X2_F> transforms_;
};

// Obrazek RGBA malowany raz - odpowiednik art.Paint.
class Paint {
public:
    Paint(int w, int h, Pt origin = {0, 0});
    ~Paint();
    Paint(const Paint&) = delete;
    Paint& operator=(const Paint&) = delete;

    void fill(const Shape& s, RGB color, int alpha = 255);
    void stroke(const Shape& s, RGB color, float width, int alpha = 255);  // po wewnetrznej stronie
    void gradient(const Shape& s, RGB top, RGB bottom, int alpha = 255);
    void gradient(const Shape& s, RGB top, RGB bottom, float y0, float y1, int alpha = 255);
    void sheen(const Shape& s, int alpha = 40, float frac = 0.5f, RGB color = WHITE);
    void radial(float cx, float cy, float r, RGB inner, RGB outer, int alpha_in = 255, int alpha_out = 0);
    void line(const std::vector<Pt>& pts, RGB color, float width, int alpha = 255, bool round_caps = true);
    void arc(float cx, float cy, float r, float a0, float a1, RGB color, float width, int alpha = 255);
    void text(float x, float y, std::string_view s, float size, RGB color, bool bold = false, int alpha = 255);
    void shadow(const Shape& s, float dy = 4, float blur = 6, int alpha = 150, RGB color = BLACK);
    void glow(const Shape& s, RGB color, float radius = 6, int alpha = 170);

    void push_clip(const Shape& s);
    void push_clip_rect(float x1, float y1, float x2, float y2);
    void pop_clip();

    Sprite finish();

    int width() const { return w_; }
    int height() const { return h_; }

private:
    int w_, h_;
    Pt origin_;
    ComPtr<ID2D1BitmapRenderTarget> rt_;
    std::vector<bool> clip_layers_;
    bool finished_ = false;
};

// Wyglad jednej kontrolki w zwyklym stanie i po wcisnieciu.
struct CapStyle {
    RGB top, bottom;      // gradient plastiku
    RGB edge;             // cienki obrys
    RGB on;               // kolor wcisnietej
    RGB glyph, glyph_on;  // kolor napisu/piktogramu i napisu na wcisnietej
    RGB accent;           // kolor obwodki przypisanej/wskazanej
    RGB warn;             // kolor nauki
    RGB ink;              // ciemny napis na jasnym tle
    bool gloss = true;    // polysk wypuklego plastiku
    bool has_assigned = false;
    RGB assigned = 0;     // obrys przypisanej kontrolki
};

using Glyph = std::function<void(Paint&, RGB)>;

Sprite render_cap(const Shape& shape, const std::string& state, const CapStyle& style, const Glyph& glyph,
                  bool shadow);

} // namespace wb::ui

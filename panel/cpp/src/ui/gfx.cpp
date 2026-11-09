#include "gfx.h"

#include <dwrite_1.h>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "../util.h"

namespace wb::ui {

namespace {

constexpr float PI = std::numbers::pi_v<float>;
constexpr int GLOW_MARGIN = 22;      // zapas na poswiate wokol kontrolki
constexpr int SHADOW_MARGIN = 10;    // bez poswiaty wystarczy zapas na cien

D2D1_COLOR_F color_f(RGB c, float a = 1.0f) {
    return D2D1::ColorF(((c >> 16) & 0xFF) / 255.0f, ((c >> 8) & 0xFF) / 255.0f, (c & 0xFF) / 255.0f, a);
}

ComPtr<ID2D1PathGeometry> polygon_geometry(ID2D1Factory* factory, const std::vector<Pt>& pts, bool closed) {
    ComPtr<ID2D1PathGeometry> path;
    factory->CreatePathGeometry(&path);
    ComPtr<ID2D1GeometrySink> sink;
    path->Open(&sink);
    if (!pts.empty()) {
        sink->BeginFigure(D2D1::Point2F(pts[0].x, pts[0].y), closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        for (size_t i = 1; i < pts.size(); ++i) sink->AddLine(D2D1::Point2F(pts[i].x, pts[i].y));
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    }
    sink->Close();
    return path;
}

std::vector<Pt> arc_points(float cx, float cy, float r, float a0, float a1) {
    int steps = std::max(8, static_cast<int>(std::abs(a1 - a0) * r / 90.0f) + 8);
    std::vector<Pt> pts;
    pts.reserve(static_cast<size_t>(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        float a = (a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps)) * PI / 180.0f;
        pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    return pts;
}

// Pokrycie ksztaltu co piksel (1 = srodek piksela w ksztalcie).
void rasterize(const Shape& s, int ox, int oy, int w, int h, std::vector<float>& out) {
    out.assign(static_cast<size_t>(w) * h, 0.0f);
    if (s.kind == Shape::Poly) {
        // skanowanie wierszami - zwykle "contains" dla korpusu pada to
        // setki milionow porownan
        std::vector<float> xs;
        size_t n = s.pts.size();
        for (int row = 0; row < h; ++row) {
            float y = static_cast<float>(row + oy) + 0.5f;
            xs.clear();
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                const Pt& a = s.pts[i];
                const Pt& b = s.pts[j];
                if ((a.y > y) != (b.y > y)) xs.push_back(a.x + (b.x - a.x) * (y - a.y) / (b.y - a.y));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                int x1 = std::max(0, static_cast<int>(std::ceil(xs[k] - 0.5f)) - ox);
                int x2 = std::min(w, static_cast<int>(std::ceil(xs[k + 1] - 0.5f)) - ox);
                for (int x = x1; x < x2; ++x) out[static_cast<size_t>(row) * w + x] = 1.0f;
            }
        }
        return;
    }
    for (int row = 0; row < h; ++row)
        for (int x = 0; x < w; ++x)
            if (s.contains(static_cast<float>(x + ox) + 0.5f, static_cast<float>(row + oy) + 0.5f))
                out[static_cast<size_t>(row) * w + x] = 1.0f;
}

void box_blur(std::vector<float>& data, int w, int h, int radius) {
    if (radius <= 0) return;
    std::vector<float> tmp(data.size());
    float norm = 1.0f / static_cast<float>(radius * 2 + 1);
    for (int y = 0; y < h; ++y) {
        float sum = 0.0f;
        const float* row = &data[static_cast<size_t>(y) * w];
        float* dst = &tmp[static_cast<size_t>(y) * w];
        for (int x = -radius; x <= radius; ++x) sum += row[std::clamp(x, 0, w - 1)];
        for (int x = 0; x < w; ++x) {
            dst[x] = sum * norm;
            sum += row[std::min(x + radius + 1, w - 1)] - row[std::max(x - radius, 0)];
        }
    }
    for (int x = 0; x < w; ++x) {
        float sum = 0.0f;
        for (int y = -radius; y <= radius; ++y) sum += tmp[static_cast<size_t>(std::clamp(y, 0, h - 1)) * w + x];
        for (int y = 0; y < h; ++y) {
            data[static_cast<size_t>(y) * w + x] = sum * norm;
            sum += tmp[static_cast<size_t>(std::min(y + radius + 1, h - 1)) * w + x] -
                   tmp[static_cast<size_t>(std::max(y - radius, 0)) * w + x];
        }
    }
}

} // namespace

RGB mix(RGB a, RGB b, double t) {
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    RGB out = 0;
    for (int shift : {16, 8, 0}) {
        int ca = static_cast<int>((a >> shift) & 0xFF);
        int cb = static_cast<int>((b >> shift) & 0xFF);
        int v = static_cast<int>(std::lrint(ca + (cb - ca) * t));
        out |= static_cast<RGB>(v) << shift;
    }
    return out;
}

// --------------------------------------------------------------- ksztalty

Shape Shape::circle(float cx, float cy, float r) {
    Shape s;
    s.kind = Circle;
    s.a = cx; s.b = cy; s.c = r;
    return s;
}

Shape Shape::rrect(float x1, float y1, float x2, float y2, float r) {
    Shape s;
    s.kind = RRect;
    s.a = x1; s.b = y1; s.c = x2; s.d = y2; s.e = r;
    return s;
}

Shape Shape::poly(std::vector<Pt> points) {
    Shape s;
    s.kind = Poly;
    s.pts = std::move(points);
    return s;
}

Shape Shape::ring(float cx, float cy, float ro, float ri, float a0, float a1) {
    Shape s;
    s.kind = Ring;
    s.a = cx; s.b = cy; s.c = ro; s.d = ri; s.e = a0; s.f = a1;
    return s;
}

std::vector<Pt> ring_points(float cx, float cy, float ro, float ri, float a0, float a1, int steps) {
    std::vector<Pt> pts;
    pts.reserve(static_cast<size_t>(steps) * 2 + 2);
    for (int i = 0; i <= steps; ++i) {
        float a = (a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps)) * PI / 180.0f;
        pts.push_back({cx + ro * std::cos(a), cy + ro * std::sin(a)});
    }
    for (int i = steps; i >= 0; --i) {
        float a = (a0 + (a1 - a0) * static_cast<float>(i) / static_cast<float>(steps)) * PI / 180.0f;
        pts.push_back({cx + ri * std::cos(a), cy + ri * std::sin(a)});
    }
    return pts;
}

void Shape::bbox(float& x1, float& y1, float& x2, float& y2) const {
    switch (kind) {
    case Circle:
        x1 = a - c; y1 = b - c; x2 = a + c; y2 = b + c;
        return;
    case RRect:
        x1 = a; y1 = b; x2 = c; y2 = d;
        return;
    case Ring:
        if (f - e >= 360.0f) {
            x1 = a - c; y1 = b - c; x2 = a + c; y2 = b + c;
            return;
        }
        [[fallthrough]];
    case Poly: {
        const std::vector<Pt> list = kind == Poly ? pts : ring_points(a, b, c, d, e, f, 64);
        x1 = y1 = 1e9f;
        x2 = y2 = -1e9f;
        for (const Pt& p : list) {
            x1 = std::min(x1, p.x); y1 = std::min(y1, p.y);
            x2 = std::max(x2, p.x); y2 = std::max(y2, p.y);
        }
        return;
    }
    }
}

Shape Shape::grow(float g) const {
    switch (kind) {
    case Circle: return circle(a, b, std::max(0.5f, c + g));
    case RRect: return rrect(a - g, b - g, c + g, d + g, std::max(0.0f, e + g));
    case Ring: {
        float span = std::max(1.0f, (c + d) / 2);
        float da = g / span * 180.0f / PI;
        return ring(a, b, c + g, std::max(0.0f, d - g), e - da, f + da);
    }
    case Poly: {
        // odsuniecie wierzcholkow od srodka ciezkosci - wystarcza dla
        // wypuklych ksztaltow
        float cx = 0, cy = 0;
        for (const Pt& p : pts) { cx += p.x; cy += p.y; }
        cx /= static_cast<float>(pts.size());
        cy /= static_cast<float>(pts.size());
        std::vector<Pt> out;
        out.reserve(pts.size());
        for (const Pt& p : pts) {
            float dx = p.x - cx, dy = p.y - cy;
            float n = std::hypot(dx, dy);
            if (n == 0) n = 1;
            out.push_back({p.x + dx / n * g, p.y + dy / n * g});
        }
        return poly(std::move(out));
    }
    }
    return *this;
}

bool Shape::contains(float x, float y) const {
    switch (kind) {
    case Circle: return std::hypot(x - a, y - b) <= c;
    case RRect: {
        if (!(a <= x && x <= c && b <= y && y <= d)) return false;
        float r = std::min({e, (c - a) / 2, (d - b) / 2});
        float cx = std::min(std::max(x, a + r), c - r);
        float cy = std::min(std::max(y, b + r), d - r);
        return std::hypot(x - cx, y - cy) <= r + 1e-6f;
    }
    case Ring: {
        float dist = std::hypot(x - a, y - b);
        if (!(d <= dist && dist <= c)) return false;
        float deg = std::atan2(y - b, x - a) * 180.0f / PI;
        while (deg < e) deg += 360.0f;
        while (deg > e + 360.0f) deg -= 360.0f;
        return deg <= f;
    }
    case Poly: {
        bool hit = false;
        size_t n = pts.size();
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            const Pt& pi = pts[i];
            const Pt& pj = pts[j];
            if ((pi.y > y) != (pj.y > y) && x < (pj.x - pi.x) * (y - pi.y) / (pj.y - pi.y) + pi.x) hit = !hit;
        }
        return hit;
    }
    }
    return false;
}

// -------------------------------------------------------------------- Gfx

Gfx& Gfx::get() {
    static Gfx instance;
    return instance;
}

bool Gfx::init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                 reinterpret_cast<void**>(factory_.GetAddressOf()))))
        return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()))))
        return false;

    D2D1_STROKE_STYLE_PROPERTIES props = D2D1::StrokeStyleProperties(
        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(props, nullptr, 0, &round_);
    props = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                        D2D1_LINE_JOIN_ROUND);
    factory_->CreateStrokeStyle(props, nullptr, 0, &flat_);

    // Ikony zakladek: czcionka Windows 11, a na Windows 10 jej poprzedniczka
    // o tych samych kodach. Bez nich - same napisy.
    ComPtr<IDWriteFontCollection> fonts;
    dwrite_->GetSystemFontCollection(&fonts);
    for (const wchar_t* name : {L"Segoe Fluent Icons", L"Segoe MDL2 Assets"}) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        fonts->FindFamilyName(name, &index, &exists);
        if (exists) {
            icon_family_ = name;
            break;
        }
    }

    auto pick = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        for (const wchar_t* name : names) {
            UINT32 index = 0;
            BOOL exists = FALSE;
            fonts->FindFamilyName(name, &index, &exists);
            if (exists) return name;
        }
        return L"Segoe UI";
    };
    // Bahnschrift to kroj DIN - z tablic i zegarow samochodowych.
    static const std::wstring ui = pick({L"Segoe UI Variable Text", L"Segoe UI"});
    display_family_ = pick({L"Bahnschrift", L"Segoe UI Semibold", L"Segoe UI"});
    static const std::wstring mono = pick({L"Cascadia Mono", L"Consolas"});
    static const std::wstring icons = icon_family_.empty() ? ui : icon_family_;

    struct Spec { const std::wstring* family; float px; DWRITE_FONT_WEIGHT weight; };
    const auto regular = DWRITE_FONT_WEIGHT_NORMAL;
    const auto strong = DWRITE_FONT_WEIGHT_SEMI_BOLD;
    const Spec specs[static_cast<int>(Font::Count)] = {
        {&ui, 13.5f, regular}, {&ui, 13.5f, strong}, {&ui, 12.5f, regular}, {&ui, 12.5f, strong},
        {&ui, 11.5f, regular}, {&ui, 11.5f, strong}, {&ui, 10.5f, strong},
        {&display_family_, 16, strong}, {&display_family_, 27, strong},
        {&display_family_, 15, strong}, {&display_family_, 23, strong},
        {&display_family_, 36, strong}, {&display_family_, 60, strong},
        {&mono, 12.5f, regular}, {&mono, 11.5f, regular}, {&mono, 12.5f, strong},
        {&icons, 16, regular}, {&icons, 12, regular}, {&icons, 22, regular},
        {&ui, 8, strong},
    };
    formats_.resize(static_cast<size_t>(Font::Count));
    for (int i = 0; i < static_cast<int>(Font::Count); ++i) {
        const Spec& sp = specs[i];
        dwrite_->CreateTextFormat(sp.family->c_str(), nullptr, sp.weight, DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, sp.px, L"pl-pl", &formats_[static_cast<size_t>(i)]);
    }
    // Cyfry o stalej szerokosci - liczby na zywo nie skacza.
    dwrite_->CreateTypography(&tabular_);
    if (tabular_) tabular_->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
    return true;
}

void Gfx::set_target(ID2D1RenderTarget* rt) {
    if (rt_ != rt) solid_.Reset();
    rt_ = rt;
}

void Gfx::drop_resources() {
    solid_.Reset();
    sprites_.clear();
    gradients_.clear();
    ++generation_;
}

ComPtr<ID2D1SolidColorBrush> Gfx::brush(RGB color, float alpha) {
    if (!solid_) {
        rt_->CreateSolidColorBrush(color_f(color, alpha), &solid_);
    } else {
        solid_->SetColor(color_f(color, alpha));
    }
    return solid_;
}

ComPtr<ID2D1StrokeStyle> Gfx::round_stroke() { return round_; }
ComPtr<ID2D1StrokeStyle> Gfx::flat_stroke() { return flat_; }
IDWriteTextFormat* Gfx::format(Font font) { return formats_[static_cast<size_t>(font)].Get(); }

ComPtr<ID2D1Geometry> Gfx::geometry(const Shape& s) {
    ComPtr<ID2D1Geometry> out;
    switch (s.kind) {
    case Shape::Circle: {
        ComPtr<ID2D1EllipseGeometry> g;
        factory_->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(s.a, s.b), s.c, s.c), &g);
        g.As(&out);
        break;
    }
    case Shape::RRect: {
        float r = std::max(0.0f, std::min({s.e, (s.c - s.a) / 2, (s.d - s.b) / 2}));
        ComPtr<ID2D1RoundedRectangleGeometry> g;
        factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(s.a, s.b, s.c, s.d), r, r), &g);
        g.As(&out);
        break;
    }
    case Shape::Poly:
        polygon_geometry(factory_.Get(), s.pts, true).As(&out);
        break;
    case Shape::Ring:
        polygon_geometry(factory_.Get(), ring_points(s.a, s.b, s.c, s.d, s.e, s.f, 96), true).As(&out);
        break;
    }
    return out;
}

void Gfx::clear(RGB color) { rt_->Clear(color_f(color)); }

void Gfx::fill_rect(const Rect& r, RGB color, float alpha) {
    rt_->FillRectangle(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), brush(color, alpha).Get());
}

void Gfx::fill_rrect(const Rect& r, float radius, RGB color, float alpha) {
    radius = std::min({radius, r.w / 2, r.h / 2});
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), radius, radius),
                              brush(color, alpha).Get());
}

void Gfx::stroke_rrect(const Rect& r, float radius, RGB color, float width, float alpha) {
    float inset = width / 2;
    D2D1_RECT_F rc = D2D1::RectF(r.x + inset, r.y + inset, r.right() - inset, r.bottom() - inset);
    float rad = std::max(0.0f, radius - inset);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rc, rad, rad), brush(color, alpha).Get(), width);
}

void Gfx::fill_circle(float cx, float cy, float r, RGB color, float alpha) {
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush(color, alpha).Get());
}

void Gfx::stroke_circle(float cx, float cy, float r, RGB color, float width, float alpha) {
    rt_->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush(color, alpha).Get(), width);
}

void Gfx::line(float x1, float y1, float x2, float y2, RGB color, float width, float alpha) {
    rt_->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush(color, alpha).Get(), width);
}

void Gfx::polyline(const std::vector<Pt>& pts, RGB color, float width, float alpha) {
    if (pts.size() < 2) return;
    auto path = polygon_geometry(factory_.Get(), pts, false);
    rt_->DrawGeometry(path.Get(), brush(color, alpha).Get(), width, round_.Get());
}

void Gfx::fill_poly(const std::vector<Pt>& pts, RGB color, float alpha) {
    if (pts.size() < 3) return;
    auto path = polygon_geometry(factory_.Get(), pts, true);
    rt_->FillGeometry(path.Get(), brush(color, alpha).Get());
}

void Gfx::arc(float cx, float cy, float r, float a0, float a1, RGB color, float width, float alpha) {
    polyline(arc_points(cx, cy, r, a0, a1), color, width, alpha);
}

void Gfx::fill_shape(const Shape& s, RGB color, float alpha) {
    rt_->FillGeometry(geometry(s).Get(), brush(color, alpha).Get());
}

void Gfx::draw_sprite(const Sprite& s, float dx, float dy) {
    if (!s) return;
    rt_->DrawBitmap(s.bitmap.Get(), D2D1::RectF(s.x + dx, s.y + dy, s.x + dx + s.w, s.y + dy + s.h));
}

void Gfx::draw_sprite_rotated(const Sprite& s, float cx, float cy, float deg) {
    if (!s) return;
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    rt_->SetTransform(D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(cx, cy)) * old);
    rt_->DrawBitmap(s.bitmap.Get(), D2D1::RectF(cx - s.w / 2, cy - s.h / 2, cx + s.w / 2, cy + s.h / 2), 1.0f,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    rt_->SetTransform(old);
}

ComPtr<IDWriteTextLayout> Gfx::layout(std::string_view s, Font font, float max_w, Align align, bool wrap, bool trim) {
    std::string key;
    key.reserve(s.size() + 24);
    key += static_cast<char>('A' + static_cast<int>(font));
    key += static_cast<char>('0' + static_cast<int>(align));
    key += wrap ? 'w' : (trim ? 't' : 'n');
    key += std::to_string(static_cast<int>(max_w * 4));
    key += '|';
    key += s;
    auto it = layouts_.find(key);
    if (it != layouts_.end()) return it->second;
    if (layouts_.size() > 4000) layouts_.clear();  // liczniki zmieniaja sie co klatke

    std::wstring text = widen(s);
    ComPtr<IDWriteTextLayout> out;
    dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format(font),
                              max_w > 0 ? max_w : 8192.0f, 8192.0f, &out);
    if (out) {
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetTextAlignment(align == Align::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                              : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                       : DWRITE_TEXT_ALIGNMENT_TRAILING);
        if (trim) {
            ComPtr<IDWriteInlineObject> sign;
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(format(font), &sign))) out->SetTrimming(&trimming, sign.Get());
        }
        DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.size())};
        if (font == Font::Num || font == Font::NumLarge || font == Font::NumHuge || font == Font::NumGiant) {
            if (tabular_) out->SetTypography(tabular_.Get(), all);
        } else if (font == Font::Caps) {
            ComPtr<IDWriteTextLayout1> spaced;
            if (SUCCEEDED(out.As(&spaced))) spaced->SetCharacterSpacing(0.0f, 0.9f, 0.0f, all);
        }
        layouts_[key] = out;
    }
    return out;
}

void Gfx::text(const Rect& r, std::string_view s, Font font, RGB color, Align align, bool wrap, float alpha) {
    if (s.empty()) return;
    auto l = layout(s, font, r.w, align, wrap);
    if (!l) return;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    float y = r.y + std::max(0.0f, (r.h - m.height) / 2);
    rt_->DrawTextLayout(D2D1::Point2F(r.x, y), l.Get(), brush(color, alpha).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Gfx::text_fit(const Rect& r, std::string_view s, Font font, RGB color, Align align, float alpha) {
    if (s.empty()) return;
    // zapas na zaokraglenie prostokata do pikseli - bez niego napis mierzony
    // "na styk" dostawalby wielokropek
    if (measure(s, font).width <= r.w + 1.5f) {
        Rect box = r;
        box.w += 2.0f;
        if (align == Align::Right) box.x -= 2.0f;
        else if (align == Align::Center) box.x -= 1.0f;
        text(box, s, font, color, align, false, alpha);
        return;
    }
    auto l = layout(s, font, r.w, align, false, true);
    if (!l) return;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    float y = r.y + std::max(0.0f, (r.h - m.height) / 2);
    rt_->DrawTextLayout(D2D1::Point2F(r.x, y), l.Get(), brush(color, alpha).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Gfx::text_at(float x, float y, std::string_view s, Font font, RGB color, float alpha) {
    auto size = measure(s, font);
    text(Rect{x - size.width / 2 - 1, y - size.height / 2, size.width + 2, size.height}, s, font, color, Align::Center, false, alpha);
}

D2D1_SIZE_F Gfx::measure(std::string_view s, Font font, float max_w) {
    if (s.empty()) {
        auto l = layout("A", font, 0, Align::Left, false);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        return D2D1::SizeF(0, m.height);
    }
    auto l = layout(s, font, max_w, Align::Left, max_w > 0);
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    return D2D1::SizeF(m.widthIncludingTrailingWhitespace, m.height);
}

void Gfx::push_clip(const Rect& r) {
    rt_->PushAxisAlignedClip(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), D2D1_ANTIALIAS_MODE_ALIASED);
}

void Gfx::pop_clip() { rt_->PopAxisAlignedClip(); }

void Gfx::push_rounded_clip(const Rect& r, float radius) {
    ComPtr<ID2D1Layer> layer;
    rt_->CreateLayer(&layer);
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry(Shape::rrect(r.x, r.y, r.right(), r.bottom(), radius)).Get()), layer.Get());
}

void Gfx::pop_rounded_clip() { rt_->PopLayer(); }

void Gfx::begin_opacity(float alpha) {
    ComPtr<ID2D1Layer> layer;
    rt_->CreateLayer(&layer);
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), layer.Get());
}

void Gfx::end_opacity() { rt_->PopLayer(); }

void Gfx::push_translate(float dx, float dy) {
    D2D1_MATRIX_3X2_F old;
    rt_->GetTransform(&old);
    transforms_.push_back(old);
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * old);
}

void Gfx::pop_translate() {
    rt_->SetTransform(transforms_.back());
    transforms_.pop_back();
}

void Gfx::spinner(float cx, float cy, float r, RGB color, double time, float width) {
    float start = static_cast<float>(std::fmod(time * 360.0, 360.0));
    stroke_circle(cx, cy, r, color, width, 0.2f);
    arc(cx, cy, r, start, start + 100.0f, color, width);
}

ComPtr<ID2D1LinearGradientBrush> Gfx::linear(RGB a, RGB b, float alpha_a, float alpha_b, D2D1_POINT_2F p0, D2D1_POINT_2F p1) {
    std::string key = std::to_string(a) + "/" + std::to_string(b) + "/" + std::to_string(static_cast<int>(alpha_a * 1000)) + "/" + std::to_string(static_cast<int>(alpha_b * 1000));
    auto it = gradients_.find(key);
    if (it == gradients_.end()) {
        D2D1_GRADIENT_STOP stops[2] = {{0.0f, color_f(a, alpha_a)}, {1.0f, color_f(b, alpha_b)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        rt_->CreateGradientStopCollection(stops, 2, &collection);
        ComPtr<ID2D1LinearGradientBrush> created;
        rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(p0, p1), collection.Get(), &created);
        it = gradients_.emplace(std::move(key), std::move(created)).first;
    }
    it->second->SetStartPoint(p0);
    it->second->SetEndPoint(p1);
    return it->second;
}

void Gfx::fill_rrect_v(const Rect& r, float radius, RGB top, RGB bottom, float a_top, float a_bottom) {
    radius = std::min({radius, r.w / 2, r.h / 2});
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), radius, radius),
                              linear(top, bottom, a_top, a_bottom, D2D1::Point2F(0, r.y), D2D1::Point2F(0, r.bottom())).Get());
}

void Gfx::fill_rrect_h(const Rect& r, float radius, RGB left, RGB right, float a_left, float a_right) {
    radius = std::min({radius, r.w / 2, r.h / 2});
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.x, r.y, r.right(), r.bottom()), radius, radius),
                              linear(left, right, a_left, a_right, D2D1::Point2F(r.x, 0), D2D1::Point2F(r.right(), 0)).Get());
}

void Gfx::fill_poly_v(const std::vector<Pt>& pts, float y0, float y1, RGB top, RGB bottom, float a_top, float a_bottom) {
    if (pts.size() < 3) return;
    auto path = polygon_geometry(factory_.Get(), pts, true);
    rt_->FillGeometry(path.Get(), linear(top, bottom, a_top, a_bottom, D2D1::Point2F(0, y0), D2D1::Point2F(0, y1)).Get());
}

void Gfx::stroke_rrect_v(const Rect& r, float radius, RGB top, RGB bottom, float a_top, float a_bottom, float width) {
    float inset = width / 2;
    D2D1_RECT_F rc = D2D1::RectF(r.x + inset, r.y + inset, r.right() - inset, r.bottom() - inset);
    float rad = std::max(0.0f, radius - inset);
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(rc, rad, rad),
                              linear(top, bottom, a_top, a_bottom, D2D1::Point2F(0, r.y), D2D1::Point2F(0, r.bottom())).Get(), width);
}

void Gfx::fill_radial(const Rect& clip, float radius, float cx, float cy, float r, RGB inner, RGB outer, float a_inner, float a_outer) {
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, color_f(inner, a_inner)}, {1.0f, color_f(outer, a_outer)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    rt_->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1RadialGradientBrush> b;
    rt_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r, r), collection.Get(), &b);
    rt_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(clip.x, clip.y, clip.right(), clip.bottom()), radius, radius), b.Get());
}

void Gfx::shadow(const Rect& rc, float radius, float blur, RGB color, float alpha, float dy) {
    float m = std::ceil(blur * 2.6f);
    float corner = std::ceil(radius) + m;
    int size = static_cast<int>(corner) * 2 + 2;
    const Sprite& s = cached("shadow/" + std::to_string(static_cast<int>(radius * 2)) + "/" + std::to_string(static_cast<int>(blur * 2)) + "/" + std::to_string(color), [&] {
        std::vector<float> mask;
        float end = static_cast<float>(size) - m;
        rasterize(Shape::rrect(m, m, end, end, radius), 0, 0, size, size, mask);
        int box = std::max(1, static_cast<int>(blur * 3.0f * std::sqrt(2.0f * PI) / 4.0f / 2.0f + 0.5f));
        for (int i = 0; i < 3; ++i) box_blur(mask, size, size, box);
        std::vector<uint32_t> pixels(mask.size());
        uint32_t cr = (color >> 16) & 0xFF, cg = (color >> 8) & 0xFF, cb = color & 0xFF;
        for (size_t i = 0; i < mask.size(); ++i) {
            float a = std::min(1.0f, mask[i]);
            pixels[i] = (static_cast<uint32_t>(a * 255.0f + 0.5f) << 24) | (static_cast<uint32_t>(static_cast<float>(cr) * a) << 16) |
                        (static_cast<uint32_t>(static_cast<float>(cg) * a) << 8) | static_cast<uint32_t>(static_cast<float>(cb) * a);
        }
        Sprite out;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        rt_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(size), static_cast<UINT32>(size)), pixels.data(), static_cast<UINT32>(size) * 4, props, &out.bitmap);
        out.w = out.h = static_cast<float>(size);
        return out;
    });
    if (!s) return;

    float fsize = static_cast<float>(size);
    float xs[4] = {rc.x - m, rc.x - m + corner, rc.right() + m - corner, rc.right() + m};
    float ys[4] = {rc.y - m + dy, rc.y - m + corner + dy, rc.bottom() + m - corner + dy, rc.bottom() + m + dy};
    float us[4] = {0, corner, fsize - corner, fsize};
    if (xs[2] < xs[1]) xs[1] = xs[2] = (xs[0] + xs[3]) / 2;
    if (ys[2] < ys[1]) ys[1] = ys[2] = (ys[0] + ys[3]) / 2;
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) {
            if (xs[i + 1] <= xs[i] || ys[j + 1] <= ys[j]) continue;
            D2D1_RECT_F dst = D2D1::RectF(xs[i], ys[j], xs[i + 1], ys[j + 1]);
            D2D1_RECT_F src = D2D1::RectF(us[i], us[j], us[i + 1], us[j + 1]);
            rt_->DrawBitmap(s.bitmap.Get(), dst, alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, src);
        }
}

void Gfx::backdrop(float x, float y) {
    const Sprite& s = cached("backdrop", [&] {
        // Slaba poswiata na ciemnym tle daje przy 8 bitach widoczne pasy -
        // losowy szum o amplitudzie jednego stopnia jasnosci je rozbija.
        const int W = 1900, H = 1200;
        std::vector<uint32_t> pixels(static_cast<size_t>(W) * H);
        uint32_t seed = 0x1234567;
        auto channel = [](RGB c, int shift) { return static_cast<double>((c >> shift) & 0xFF); };
        for (int py = 0; py < H; ++py)
            for (int px = 0; px < W; ++px) {
                double dx = (px - 1150.0) / 820.0, dy = (py + 120.0) / 560.0;
                double glow = 0.085 * std::exp(-(dx * dx + dy * dy) * 1.9);
                double vx = (px - 150.0) / 640.0, vy = (py - 1050.0) / 520.0;
                double violet = 0.030 * std::exp(-(vx * vx + vy * vy) * 1.9);
                double fade = std::min(1.0, (W - px) / 320.0) * std::min(1.0, (H - py) / 240.0);
                glow *= fade;
                violet *= fade;
                uint32_t pixel = 0xFF000000u;
                for (int shift : {16, 8, 0}) {
                    double v = channel(BG, shift) + (channel(ACCENT, shift) - channel(BG, shift)) * glow + (channel(VIOLET, shift) - channel(BG, shift)) * violet;
                    seed = seed * 1664525u + 1013904223u;
                    v += static_cast<double>(seed >> 24) / 255.0 - 0.5;
                    pixel |= static_cast<uint32_t>(std::clamp(static_cast<int>(v + 0.5), 0, 255)) << shift;
                }
                pixels[static_cast<size_t>(py) * W + px] = pixel;
            }
        Sprite out;
        D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        rt_->CreateBitmap(D2D1::SizeU(W, H), pixels.data(), W * 4, props, &out.bitmap);
        out.w = static_cast<float>(W);
        out.h = static_cast<float>(H);
        return out;
    });
    draw_sprite(s, x, y);
}

const Sprite& Gfx::cached(const std::string& key, const std::function<Sprite()>& render) {
    auto it = sprites_.find(key);
    if (it != sprites_.end()) return it->second;
    return sprites_.emplace(key, render()).first->second;
}

// ------------------------------------------------------------------ Paint

Paint::Paint(int w, int h, Pt origin) : w_(std::max(1, w)), h_(std::max(1, h)), origin_(origin) {
    Gfx& g = Gfx::get();
    g.target()->CreateCompatibleRenderTarget(D2D1::SizeF(static_cast<float>(w_), static_cast<float>(h_)), &rt_);
    rt_->BeginDraw();
    rt_->Clear(D2D1::ColorF(0, 0));
    rt_->SetTransform(D2D1::Matrix3x2F::Translation(-origin_.x, -origin_.y));
}

Paint::~Paint() {
    if (!finished_ && rt_) rt_->EndDraw();
}

void Paint::fill(const Shape& s, RGB color, int alpha) {
    Gfx& g = Gfx::get();
    ComPtr<ID2D1SolidColorBrush> b;
    rt_->CreateSolidColorBrush(color_f(color, static_cast<float>(alpha) / 255.0f), &b);
    rt_->FillGeometry(g.geometry(s).Get(), b.Get());
}

void Paint::stroke(const Shape& s, RGB color, float width, int alpha) {
    Gfx& g = Gfx::get();
    ComPtr<ID2D1SolidColorBrush> b;
    rt_->CreateSolidColorBrush(color_f(color, static_cast<float>(alpha) / 255.0f), &b);
    if (s.kind == Shape::Poly) {
        // Wielokat bywa wklesly - linia o podwojnej grubosci wzdluz krawedzi,
        // przycieta do wnetrza ksztaltu, daje rowny obrys.
        push_clip(s);
        std::vector<Pt> closed = s.pts;
        closed.push_back(s.pts.front());
        rt_->DrawGeometry(polygon_geometry(g.factory(), closed, false).Get(), b.Get(), width * 2, g.round_stroke().Get());
        pop_clip();
    } else {
        rt_->DrawGeometry(g.geometry(s.grow(-width / 2)).Get(), b.Get(), width);
    }
}

void Paint::gradient(const Shape& s, RGB top, RGB bottom, int alpha) {
    float x1, y1, x2, y2;
    s.bbox(x1, y1, x2, y2);
    gradient(s, top, bottom, y1, y2, alpha);
}

void Paint::gradient(const Shape& s, RGB top, RGB bottom, float y0, float y1, int alpha) {
    float a = static_cast<float>(alpha) / 255.0f;
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, color_f(top, a)}, {1.0f, color_f(bottom, a)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    rt_->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1LinearGradientBrush> b;
    rt_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y0), D2D1::Point2F(0, y1)),
                                   collection.Get(), &b);
    rt_->FillGeometry(Gfx::get().geometry(s).Get(), b.Get());
}

void Paint::sheen(const Shape& s, int alpha, float frac, RGB color) {
    float x1, y1, x2, y2;
    s.bbox(x1, y1, x2, y2);
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, color_f(color, static_cast<float>(alpha) / 255.0f)},
                                   {1.0f, color_f(color, 0.0f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    rt_->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1LinearGradientBrush> b;
    rt_->CreateLinearGradientBrush(
        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, y1), D2D1::Point2F(0, y1 + (y2 - y1) * frac)),
        collection.Get(), &b);
    rt_->FillGeometry(Gfx::get().geometry(s).Get(), b.Get());
}

void Paint::radial(float cx, float cy, float r, RGB inner, RGB outer, int alpha_in, int alpha_out) {
    D2D1_GRADIENT_STOP stops[2] = {{0.0f, color_f(inner, static_cast<float>(alpha_in) / 255.0f)},
                                   {1.0f, color_f(outer, static_cast<float>(alpha_out) / 255.0f)}};
    ComPtr<ID2D1GradientStopCollection> collection;
    rt_->CreateGradientStopCollection(stops, 2, &collection);
    ComPtr<ID2D1RadialGradientBrush> b;
    rt_->CreateRadialGradientBrush(
        D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r, r), collection.Get(), &b);
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), b.Get());
}

void Paint::line(const std::vector<Pt>& pts, RGB color, float width, int alpha, bool round_caps) {
    if (pts.size() < 2) return;
    Gfx& g = Gfx::get();
    ComPtr<ID2D1SolidColorBrush> b;
    rt_->CreateSolidColorBrush(color_f(color, static_cast<float>(alpha) / 255.0f), &b);
    auto path = polygon_geometry(g.factory(), pts, false);
    rt_->DrawGeometry(path.Get(), b.Get(), width, (round_caps ? g.round_stroke() : g.flat_stroke()).Get());
}

void Paint::arc(float cx, float cy, float r, float a0, float a1, RGB color, float width, int alpha) {
    if (a1 - a0 <= 0.05f) return;
    line(arc_points(cx, cy, r, a0, a1), color, width, alpha, true);
}

void Paint::text(float x, float y, std::string_view s, float size, RGB color, bool bold, int alpha) {
    Gfx& g = Gfx::get();
    std::wstring wide = widen(s);
    ComPtr<IDWriteTextFormat> format;
    g.dwrite()->CreateTextFormat(L"Segoe UI", nullptr, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                 DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pl-pl", &format);
    ComPtr<IDWriteTextLayout> layout;
    g.dwrite()->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()), format.Get(), 400.0f, 100.0f, &layout);
    if (!layout) return;
    layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    ComPtr<ID2D1SolidColorBrush> b;
    rt_->CreateSolidColorBrush(color_f(color, static_cast<float>(alpha) / 255.0f), &b);
    rt_->DrawTextLayout(D2D1::Point2F(x - 200.0f, y - m.height / 2), layout.Get(), b.Get());
}

void Paint::shadow(const Shape& s, float dy, float blur, int alpha, RGB color) {
    float bx1, by1, bx2, by2;
    s.bbox(bx1, by1, bx2, by2);
    float margin = blur * 3 + std::abs(dy) + 2;
    int ox = static_cast<int>(std::floor(bx1 - margin));
    int oy = static_cast<int>(std::floor(by1 - margin));
    int w = static_cast<int>(std::ceil(bx2 + margin)) - ox;
    int h = static_cast<int>(std::ceil(by2 + margin)) - oy;

    std::vector<float> mask;
    Shape moved = s;
    rasterize(moved, ox, oy - static_cast<int>(std::lrint(dy)), w, h, mask);
    // trzy przebiegi usredniania to praktycznie rozmycie Gaussa
    int radius = std::max(1, static_cast<int>(blur * 3.0f * std::sqrt(2.0f * PI) / 4.0f / 2.0f + 0.5f));
    for (int i = 0; i < 3; ++i) box_blur(mask, w, h, radius);

    std::vector<uint32_t> pixels(mask.size());
    uint32_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    for (size_t i = 0; i < mask.size(); ++i) {
        float a = std::min(1.0f, mask[i]) * static_cast<float>(alpha) / 255.0f;
        uint32_t ia = static_cast<uint32_t>(a * 255.0f + 0.5f);
        pixels[i] = (ia << 24) | (static_cast<uint32_t>(static_cast<float>(r) * a) << 16) |
                    (static_cast<uint32_t>(static_cast<float>(g) * a) << 8) | static_cast<uint32_t>(static_cast<float>(b) * a);
    }
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap> bmp;
    rt_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), pixels.data(),
                      static_cast<UINT32>(w) * 4, props, &bmp);
    if (!bmp) return;
    rt_->DrawBitmap(bmp.Get(), D2D1::RectF(static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(ox + w),
                                           static_cast<float>(oy + h)));
}

void Paint::glow(const Shape& s, RGB color, float radius, int alpha) {
    shadow(s.grow(radius * 0.35f), 0, radius, alpha, color);
}

void Paint::push_clip(const Shape& s) {
    ComPtr<ID2D1Layer> layer;
    rt_->CreateLayer(&layer);
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), Gfx::get().geometry(s).Get()), layer.Get());
    clip_layers_.push_back(true);
}

void Paint::push_clip_rect(float x1, float y1, float x2, float y2) {
    rt_->PushAxisAlignedClip(D2D1::RectF(x1, y1, x2, y2), D2D1_ANTIALIAS_MODE_ALIASED);
    clip_layers_.push_back(false);
}

void Paint::pop_clip() {
    if (clip_layers_.back()) rt_->PopLayer();
    else rt_->PopAxisAlignedClip();
    clip_layers_.pop_back();
}

Sprite Paint::finish() {
    finished_ = true;
    rt_->EndDraw();
    Sprite out;
    rt_->GetBitmap(&out.bitmap);
    out.x = origin_.x;
    out.y = origin_.y;
    out.w = static_cast<float>(w_);
    out.h = static_cast<float>(h_);
    return out;
}

Sprite render_cap(const Shape& shape, const std::string& state, const CapStyle& style, const Glyph& glyph, bool shadow) {
    float x1, y1, x2, y2;
    shape.bbox(x1, y1, x2, y2);
    bool glowing = state == "active" || state == "learn_on" || state == "hover";
    float m = static_cast<float>(glowing ? GLOW_MARGIN : SHADOW_MARGIN);
    float ox = std::floor(x1 - m), oy = std::floor(y1 - m);
    int w = static_cast<int>(std::ceil(x2 + m) - ox), h = static_cast<int>(std::ceil(y2 + m) - oy);
    Paint p(w, h, {ox, oy});

    if (state == "active" || state == "learn_on")
        p.glow(shape, state == "learn_on" ? style.warn : style.on, 6, 210);
    else if (state == "hover")
        p.glow(shape, style.accent, 5, 140);
    if (shadow) p.shadow(shape, 2.5f, 2.5f, 160);

    RGB glyph_color;
    if (state == "active") {
        p.gradient(shape, lighten(style.on, 0.22), darken(style.on, 0.10));
        glyph_color = style.glyph_on;
    } else if (state == "learn_on") {
        p.gradient(shape, lighten(style.warn, 0.18), darken(style.warn, 0.08));
        glyph_color = style.ink;
    } else {
        p.gradient(shape, style.top, style.bottom);
        glyph_color = style.glyph;
        // odblask na gornej polowie - wypukly plastik
        if (style.gloss) {
            p.sheen(shape, 34, 0.6f);
            p.stroke(shape, WHITE, 1.0f, 22);
        }
    }

    RGB edge = style.edge;
    float edge_w = 1.0f;
    int edge_a = 255;
    if (state == "learn_off") {
        edge = style.warn;
        edge_w = 2.0f;
        glyph_color = style.warn;
    } else if (state == "hover") {
        edge = style.accent;
        edge_w = 2.0f;
    } else if (state == "assigned") {
        edge_w = 1.5f;
        if (style.has_assigned) {
            edge = style.assigned;
        } else {
            edge = style.accent;
            edge_a = 170;
        }
    } else if (state == "active" || state == "learn_on") {
        edge = WHITE;
        edge_a = 70;
    }
    p.stroke(shape, edge, edge_w, edge_a);

    if (glyph) glyph(p, glyph_color);
    return p.finish();
}

} // namespace wb::ui

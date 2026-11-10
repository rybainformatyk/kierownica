#include "padview.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "../outputs/xinput.h"
#include "../util.h"

namespace wb::ui {

namespace {

constexpr float PI = std::numbers::pi_v<float>;
float rad(float deg) { return deg * PI / 180.0f; }

// --------------------------------------------------------------- rysunek pada
//
// Wszystko ponizej jest mierzone na zdjeciu (powiekszenie 3x, srodek pada
// x = 640, gora korpusu y = 185) i przeliczane jedna funkcja.

constexpr int PAD_W = 560, PAD_H = 420;
constexpr float CX = 280.0f, CY = 196.0f;
constexpr float PHOTO_S = 0.47f;
constexpr float PHOTO_X = 640.0f;
constexpr float PHOTO_Y = 185.0f + 136.0f / 0.47f;

Pt ph(float x, float y) { return {(x - PHOTO_X) * PHOTO_S, (y - PHOTO_Y) * PHOTO_S}; }

const std::vector<Pt>& silhouette() {
    static const std::vector<Pt> pts = [] {
        std::vector<Pt> out;
        const float raw[][2] = {{640, 185}, {700, 186}, {780, 182}, {860, 178}, {930, 185}, {970, 198}, {992, 215},
                                {1010, 250}, {1035, 310}, {1062, 390}, {1088, 470}, {1110, 550}, {1128, 630},
                                {1140, 700}, {1142, 755}, {1128, 800}, {1100, 830}, {1065, 842}, {1030, 835},
                                {1000, 815}, {965, 780}, {925, 735}, {890, 695}, {862, 664}, {840, 652},
                                {760, 650}, {640, 650}};
        for (const auto& p : raw) out.push_back(ph(p[0], p[1]));
        return out;
    }();
    return pts;
}

const Pt STICK_L = ph(375, 345), STICK_R = ph(775, 500);
const float STICK_RING = 80 * PHOTO_S;
constexpr float STICK_KNOB = 26.0f, STICK_TRAVEL = 8.0f;
const Pt DPAD_C = ph(503, 508);
const float DPAD_R = 85 * PHOTO_S;
constexpr float DPAD_HUB = 8.0f;
const Pt FACE_C = ph(903, 343);
const float FACE_OFFSET = 68 * PHOTO_S, FACE_R = 33 * PHOTO_S;
const Pt GUIDE_C = ph(640, 240);
const float GUIDE_R = 38 * PHOTO_S;
const Pt VIEW_C = ph(563, 343);
const float SMALL_R = 22 * PHOTO_S;
const Pt SHARE_C = ph(640, 397);
const float SHARE_HW = 25 * PHOTO_S, SHARE_HH = 11 * PHOTO_S;
const float LED_HALF = 11 * PHOTO_S;

constexpr float BUMPER_X0 = 54.0f, BUMPER_X1 = 184.0f;
constexpr float BUMPER_THICK_END = 2.0f, BUMPER_THICK_MAX = 17.0f;
constexpr float BUMPER_PEAK = 0.6f;
constexpr float BUMPER_INNER = -1.5f;
constexpr float TRIGGER_SPAN0 = 0.08f, TRIGGER_SPAN1 = 0.64f;
constexpr float TRIGGER_RISE = 23.0f, TRIGGER_TUCK = 3.0f;
constexpr int TRIGGER_STEPS = 24;

// paleta ze zdjecia
constexpr RGB BODY_TOP = 0x2c2e32, BODY_BOTTOM = 0x191a1d;
constexpr RGB VISOR_TOP = 0x34363a, VISOR_BOTTOM = 0x222326;
constexpr RGB GRIP = 0x24262a, RECESS = 0x0d0e10, LETTER = 0x8b8e94;
constexpr RGB INK = 0x0b0e14;

CapStyle gloss_style() {
    CapStyle s{};
    s.top = 0x2e3034; s.bottom = 0x08090a; s.edge = 0x000000; s.on = ACCENT;
    s.glyph = LETTER; s.glyph_on = ACCENT_INK; s.accent = ACCENT; s.warn = WARN; s.ink = 0x20160a;
    s.gloss = true; s.has_assigned = true; s.assigned = mix(0x000000, ACCENT, 0.30);
    return s;
}

CapStyle with(CapStyle s, RGB top, RGB bottom, RGB edge, RGB glyph, bool gloss) {
    s.top = top; s.bottom = bottom; s.edge = edge; s.glyph = glyph; s.gloss = gloss;
    return s;
}

CapStyle assigned_edge(CapStyle s, RGB edge) {
    s.has_assigned = true;
    s.assigned = mix(edge, ACCENT, 0.30);
    return s;
}

std::vector<Pt> smooth(std::vector<Pt> pts) {
    std::vector<Pt> out;
    out.push_back(pts.front());
    for (size_t i = 0; i + 2 < pts.size(); ++i)
        out.push_back({(pts[i].x + 2 * pts[i + 1].x + pts[i + 2].x) / 4, (pts[i].y + 2 * pts[i + 1].y + pts[i + 2].y) / 4});
    out.push_back(pts.back());
    return out;
}

std::vector<Pt> catmull(const std::vector<Pt>& p, int steps = 6) {
    int n = static_cast<int>(p.size());
    std::vector<Pt> out;
    out.reserve(static_cast<size_t>(n * steps));
    for (int i = 0; i < n; ++i) {
        const Pt& p0 = p[static_cast<size_t>((i - 1 + n) % n)];
        const Pt& p1 = p[static_cast<size_t>(i)];
        const Pt& p2 = p[static_cast<size_t>((i + 1) % n)];
        const Pt& p3 = p[static_cast<size_t>((i + 2) % n)];
        for (int step = 0; step < steps; ++step) {
            float t = static_cast<float>(step) / static_cast<float>(steps);
            float t2 = t * t, t3 = t2 * t;
            auto f = [&](float a0, float a1, float a2, float a3) {
                return 0.5f * (2 * a1 + (-a0 + a2) * t + (2 * a0 - 5 * a1 + 4 * a2 - a3) * t2 + (-a0 + 3 * a1 - 3 * a2 + a3) * t3);
            };
            out.push_back({f(p0.x, p1.x, p2.x, p3.x), f(p0.y, p1.y, p2.y, p3.y)});
        }
    }
    return out;
}

const std::vector<Pt>& body_outline() {
    static const std::vector<Pt> outline = [] {
        std::vector<Pt> right = smooth(silhouette());
        std::vector<Pt> all = right;
        for (size_t i = right.size() - 2; i >= 1; --i) all.push_back({-right[i].x, right[i].y});
        return catmull(all);
    }();
    return outline;
}

struct TrackPoint {
    Pt p, n;
    float thick;
};

// Punkty wzdluz barku (prawego) - od srodka pada ku bokowi, zageszczone,
// zeby odsuniete krawedzie byly gladkie i na rogu.
const std::vector<TrackPoint>& bumper_track() {
    static const std::vector<TrackPoint> track = [] {
        const auto& pts = body_outline();
        int n = static_cast<int>(pts.size());
        std::vector<std::pair<Pt, Pt>> raw;
        for (int i = 0; i < n; ++i) {
            const Pt& pt = pts[static_cast<size_t>(i)];
            if (!(BUMPER_X0 <= pt.x && pt.x <= BUMPER_X1 && pt.y < -40)) continue;
            const Pt& a = pts[static_cast<size_t>((i - 1 + n) % n)];
            const Pt& b = pts[static_cast<size_t>((i + 1) % n)];
            float tx = b.x - a.x, ty = b.y - a.y;
            float len = std::hypot(tx, ty);
            if (len == 0) len = 1;
            float nx = ty / len, ny = -tx / len;
            if (nx * pt.x + ny * (pt.y - 40.0f) < 0) { nx = -nx; ny = -ny; }  // na zewnatrz korpusu
            raw.push_back({pt, {nx, ny}});
        }
        std::vector<std::pair<Pt, Pt>> dense;
        for (size_t i = 0; i + 1 < raw.size(); ++i) {
            const auto& [a, an] = raw[i];
            const auto& [b, bn] = raw[i + 1];
            for (int s = 0; s < 4; ++s) {
                float t = static_cast<float>(s) / 4;
                float nx = an.x + (bn.x - an.x) * t, ny = an.y + (bn.y - an.y) * t;
                float nl = std::hypot(nx, ny);
                if (nl == 0) nl = 1;
                dense.push_back({{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}, {nx / nl, ny / nl}});
            }
        }
        dense.push_back(raw.back());
        // pierwiastek sinusa: tepy srodek, ostre konce; potega przesuwa
        // najgrubsze miejsce ku rogowi
        float power = std::log(0.5f) / std::log(BUMPER_PEAK);
        std::vector<TrackPoint> out;
        for (size_t k = 0; k < dense.size(); ++k) {
            float u = static_cast<float>(k) / static_cast<float>(dense.size() - 1);
            float thick = BUMPER_THICK_END + (BUMPER_THICK_MAX - BUMPER_THICK_END) * std::pow(std::sin(PI * std::pow(u, power)), 0.6f);
            out.push_back({dense[k].first, dense[k].second, thick});
        }
        return out;
    }();
    return track;
}

Pt offset_track(size_t k, float d, int sign = 1) {
    const TrackPoint& t = bumper_track()[k];
    return {static_cast<float>(sign) * (t.p.x + t.n.x * d), t.p.y + t.n.y * d};
}

std::vector<Pt> mirrored_reversed(const std::vector<Pt>& poly) {
    std::vector<Pt> out;
    out.reserve(poly.size());
    for (auto it = poly.rbegin(); it != poly.rend(); ++it) out.push_back({-it->x, it->y});
    return out;
}

std::vector<Pt> bumper_outline(int sign) {
    const auto& track = bumper_track();
    std::vector<Pt> outer, inner;
    for (size_t k = 0; k < track.size(); ++k) {
        outer.push_back(offset_track(k, std::max(0.0f, track[k].thick), 1));
        inner.push_back(offset_track(k, BUMPER_INNER));
    }
    std::vector<Pt> poly = outer;
    poly.insert(poly.end(), inner.rbegin(), inner.rend());
    return sign < 0 ? mirrored_reversed(poly) : poly;
}

Pt bumper_center(int sign) {
    const auto& track = bumper_track();
    size_t best = 0;
    for (size_t i = 1; i < track.size(); ++i)
        if (track[i].thick > track[best].thick) best = i;
    return offset_track(best, (track[best].thick + BUMPER_INNER) / 2, sign);
}

std::vector<size_t> trigger_range() {
    size_t n = bumper_track().size();
    size_t a = static_cast<size_t>(std::lrint(TRIGGER_SPAN0 * static_cast<float>(n - 1)));
    size_t b = static_cast<size_t>(std::lrint(TRIGGER_SPAN1 * static_cast<float>(n - 1)));
    std::vector<size_t> out;
    for (size_t k = a; k <= b; ++k) out.push_back(k);
    return out;
}

// Strome, zaokraglone konce i plaski grzbiet, lekko wyzszy blizej srodka.
float trigger_rise(float v) {
    float s = std::sin(PI * std::min(1.0f, std::max(0.0f, v)));
    return TRIGGER_RISE * std::pow(std::max(0.0f, s), 0.45f) * (1.0f - 0.18f * v);
}

std::vector<Pt> trigger_outline(int sign) {
    const auto& track = bumper_track();
    auto ks = trigger_range();
    std::vector<Pt> top, bottom;
    for (size_t j = 0; j < ks.size(); ++j) {
        float thick = track[ks[j]].thick;
        float v = static_cast<float>(j) / static_cast<float>(ks.size() - 1);
        top.push_back(offset_track(ks[j], thick + trigger_rise(v)));
        bottom.push_back(offset_track(ks[j], thick - std::min(TRIGGER_TUCK, thick * 0.6f)));
    }
    std::vector<Pt> poly = top;
    poly.insert(poly.end(), bottom.rbegin(), bottom.rend());
    return sign < 0 ? mirrored_reversed(poly) : poly;
}

Pt trigger_label(int sign) {
    const auto& track = bumper_track();
    auto ks = trigger_range();
    size_t best = 0;
    float best_rise = -1;
    for (size_t j = 0; j < ks.size(); ++j) {
        float rise = trigger_rise(static_cast<float>(j) / static_cast<float>(ks.size() - 1));
        if (rise > best_rise) { best_rise = rise; best = j; }
    }
    return offset_track(ks[best], track[ks[best]].thick + best_rise * 0.5f, sign);
}

std::vector<Pt> trigger_ridge(int sign, float ox, float oy) {
    const auto& track = bumper_track();
    auto ks = trigger_range();
    std::vector<Pt> out;
    for (size_t j = 0; j < ks.size(); ++j) {
        float rise = trigger_rise(static_cast<float>(j) / static_cast<float>(ks.size() - 1));
        if (rise > 0.45f * TRIGGER_RISE) {
            Pt p = offset_track(ks[j], track[ks[j]].thick + rise - 1.8f, sign);
            out.push_back({ox + p.x, oy + p.y});
        }
    }
    return out;
}

Shape offset_poly(const std::vector<Pt>& poly, float ox, float oy) {
    std::vector<Pt> out;
    out.reserve(poly.size());
    for (const Pt& p : poly) out.push_back({ox + p.x, oy + p.y});
    return Shape::poly(std::move(out));
}

// Wielokat z zaokraglonymi rogami.
std::vector<Pt> rounded_poly(const std::vector<std::array<float, 3>>& corners, int steps = 5) {
    int n = static_cast<int>(corners.size());
    std::vector<Pt> out;
    for (int i = 0; i < n; ++i) {
        auto [x, y, rr] = corners[static_cast<size_t>(i)];
        if (rr <= 0) { out.push_back({x, y}); continue; }
        const auto& prev = corners[static_cast<size_t>((i - 1 + n) % n)];
        const auto& next = corners[static_cast<size_t>((i + 1) % n)];
        auto toward = [&, x = x, y = y, rr = rr](float ax, float ay) {
            float dx = ax - x, dy = ay - y;
            float d = std::hypot(dx, dy);
            if (d == 0) d = 1;
            return Pt{x + dx / d * rr, y + dy / d * rr};
        };
        Pt a = toward(prev[0], prev[1]);
        Pt b = toward(next[0], next[1]);
        for (int s = 0; s <= steps; ++s) {
            float t = static_cast<float>(s) / static_cast<float>(steps);
            out.push_back({(1 - t) * (1 - t) * a.x + 2 * (1 - t) * t * x + t * t * b.x,
                           (1 - t) * (1 - t) * a.y + 2 * (1 - t) * t * y + t * t * b.y});
        }
    }
    return out;
}

Glyph text_glyph(float x, float y, std::string text, float size, bool bold = false) {
    return [=](Paint& p, RGB color) { p.text(x, y, text, size, color, bold); };
}

Glyph arrow_glyph(float x, float y, int ux, int uy, float size) {
    float fx = static_cast<float>(ux), fy = static_cast<float>(uy);
    float px = -fy, py = fx;
    Pt tip{x + fx * size, y + fy * size};
    Pt left{x - fx * size * 0.6f + px * size, y - fy * size * 0.6f + py * size};
    Pt right{x - fx * size * 0.6f - px * size, y - fy * size * 0.6f - py * size};
    return [=](Paint& p, RGB color) { p.fill(Shape::poly({tip, left, right}), color); };
}

Glyph stick_ticks(float x, float y) {
    return [=](Paint& p, RGB) {
        p.arc(x, y, STICK_RING - 2.0f, 200, 340, BLACK, 4.0f, 170);
        p.arc(x, y, STICK_RING - 1.0f, 25, 155, WHITE, 1.3f, 55);
        p.stroke(Shape::circle(x, y, STICK_RING), 0x4a4c50, 1.0f, 110);
    };
}

Glyph knob_glyph(float x, float y) {
    float r = STICK_KNOB;
    return [=](Paint& p, RGB color) {
        if (color == BLACK) {
            p.gradient(Shape::circle(x, y, r - 3.5f), 0x7a7d83, 0x2c2e32);
            p.gradient(Shape::circle(x, y, r - 5.5f), 0x0b0c0d, 0x2e3034);
        }
        for (int k = 0; k < 44; ++k) {
            float a = 2 * PI * static_cast<float>(k) / 44;
            float ca = std::cos(a), sa = std::sin(a);
            p.line({{x + ca * (r - 4.0f), y + sa * (r - 4.0f)}, {x + ca * (r - 1.0f), y + sa * (r - 1.0f)}}, BLACK, 1.0f, 110, false);
        }
        p.arc(x, y, r - 1.0f, 200, 300, WHITE, 1.4f, 90);
    };
}

Glyph facet_glyph(float x, float y, float a0, float a1) {
    return [=](Paint& p, RGB) {
        p.fill(Shape::ring(x, y, DPAD_R - 8.0f, DPAD_HUB + 6.0f, a0 + 9.0f, a1 - 9.0f), WHITE, 26);
        p.arc(x, y, DPAD_R - 1.2f, a0 + 2.0f, a1 - 2.0f, WHITE, 1.0f, 50);
    };
}

Glyph bumper_glyph(int sign, std::string name) {
    const auto& track = bumper_track();
    std::vector<Pt> ridge, soft;
    for (size_t k = 0; k < track.size(); ++k) {
        if (track[k].thick <= 6.0f) continue;
        Pt a = offset_track(k, track[k].thick - 2.0f, sign);
        Pt b = offset_track(k, track[k].thick * 0.55f, sign);
        ridge.push_back({CX + a.x, CY + a.y});
        soft.push_back({CX + b.x, CY + b.y});
    }
    Pt c = bumper_center(sign);
    return [=](Paint& p, RGB color) {
        p.line(soft, WHITE, 4.0f, 16);
        p.line(ridge, WHITE, 1.2f, 120);
        p.text(CX + c.x, CY + c.y + 0.5f, name, 7.5f, color, true);
    };
}

Glyph trigger_glyph(int sign, Shape shape) {
    Shape bumper = offset_poly(bumper_outline(sign), CX, CY);
    std::vector<Pt> ridge = trigger_ridge(sign, CX, CY);
    const auto& track = bumper_track();
    auto ks = trigger_range();
    std::vector<std::vector<Pt>> grooves;
    for (size_t j = 0; j < ks.size(); ++j) {
        float v = static_cast<float>(j) / static_cast<float>(ks.size() - 1);
        if (0.66f <= v && v <= 0.94f && j % 5 == 0) {
            float rise = trigger_rise(v), base = track[ks[j]].thick;
            Pt a = offset_track(ks[j], base + 3.0f, sign);
            Pt b = offset_track(ks[j], base + rise - 4.5f, sign);
            grooves.push_back({{CX + a.x, CY + a.y}, {CX + b.x, CY + b.y}});
        }
    }
    float fsign = static_cast<float>(sign);
    return [=](Paint& p, RGB) {
        p.push_clip(shape);
        p.shadow(bumper, -1.0f, 2.5f, 210);
        p.pop_clip();
        for (const auto& line : grooves) {
            p.line(line, BLACK, 1.4f, 110);
            std::vector<Pt> moved;
            for (const Pt& q : line) moved.push_back({q.x + fsign, q.y});
            p.line(moved, WHITE, 0.8f, 30);
        }
        p.line(ridge, WHITE, 1.1f, 80);
    };
}

Glyph view_glyph(float x, float y) {
    return [=](Paint& p, RGB color) {
        p.stroke(Shape::rrect(x - 4.6f, y - 3.4f, x + 1.2f, y + 1.6f, 1.0f), color, 1.2f);
        p.fill(Shape::rrect(x - 1.2f, y - 1.6f, x + 4.6f, y + 3.4f, 1.0f), color);
    };
}

Glyph menu_glyph(float x, float y) {
    return [=](Paint& p, RGB color) {
        for (float dy : {-2.8f, 0.0f, 2.8f}) p.line({{x - 3.8f, y + dy}, {x + 3.8f, y + dy}}, color, 1.3f);
    };
}

Glyph guide_glyph(float x, float y) {
    return [=](Paint& p, RGB color) {
        p.stroke(Shape::circle(x, y, GUIDE_R * 0.56f), color, 2.2f);
        p.fill(Shape::circle(x, y, GUIDE_R * 0.2f), color);
    };
}

// Wypelnienie spustu akcentem od wewnatrz na zewnatrz - step/STEPS.
Sprite render_trigger_fill(const Shape& shape, int sign, int step) {
    float x1, y1, x2, y2;
    shape.bbox(x1, y1, x2, y2);
    float ox = std::floor(x1) - 1, oy = std::floor(y1) - 1;
    Paint p(static_cast<int>(std::ceil(x2) + 1 - ox), static_cast<int>(std::ceil(y2) + 1 - oy), {ox, oy});
    float frac = static_cast<float>(step) / TRIGGER_STEPS;
    if (sign > 0) p.push_clip_rect(x1, y1 - 2, x1 + (x2 - x1) * frac, y2 + 2);
    else p.push_clip_rect(x2 - (x2 - x1) * frac, y1 - 2, x2, y2 + 2);
    p.gradient(shape, lighten(ACCENT, 0.25), darken(ACCENT, 0.15));
    p.line(trigger_ridge(sign, CX, CY), WHITE, 1.1f, 120);
    p.pop_clip();
    return p.finish();
}

void paint_pad_body(Paint& p) {
    Shape body = offset_poly(body_outline(), CX, CY);
    float top = 1e9f, bottom = -1e9f;
    for (const Pt& q : body_outline()) {
        top = std::min(top, CY + q.y);
        bottom = std::max(bottom, CY + q.y);
    }

    p.shadow(body, 10, 14, 220);
    p.gradient(body, BODY_TOP, BODY_BOTTOM, top, bottom);

    // swiatlo z gory na srodku i ciemniejsze boki - bryla, nie plyta
    p.push_clip(body);
    p.radial(CX, top - 30, 300, WHITE, WHITE, 22, 0);
    for (int sign : {-1, 1}) p.radial(CX + static_cast<float>(sign) * 250, CY + 60, 170, BLACK, BLACK, 80, 0);
    p.pop_clip();

    // gumowe uchwyty: jasniejsza czern i drobne ukosne prazki
    const float seam_raw[][2] = {{1045, 380}, {990, 460}, {935, 540}, {880, 615}, {858, 648}};
    for (int sign : {-1, 1}) {
        float s = static_cast<float>(sign);
        std::vector<Pt> seam, region;
        for (const auto& q : seam_raw) {
            Pt v = ph(q[0], q[1]);
            seam.push_back({CX + s * v.x, CY + v.y});
            region.push_back({CX + s * v.x, CY + v.y});
        }
        Pt last = ph(seam_raw[4][0], seam_raw[4][1]);
        Pt first = ph(seam_raw[0][0], seam_raw[0][1]);
        region.push_back({CX + s * 300, CY + last.y + 200});
        region.push_back({CX + s * 300, CY + first.y - 40});
        Shape grip = Shape::poly(region);

        p.push_clip(body);
        p.push_clip(grip);
        p.fill(grip, GRIP);
        float h = static_cast<float>(p.height()), w = static_cast<float>(p.width());
        float k = -s;  // prazki w strone srodka pada
        for (float x = -h; x < w + h; x += 3.2f) p.line({{x, 0}, {x + k * h * 0.6f, h}}, WHITE, 0.5f, 14, false);
        p.pop_clip();
        p.pop_clip();

        // linia podzialu: ciemny rowek i jasny brzeg obok
        p.line(seam, BLACK, 2.0f, 170);
        std::vector<Pt> moved;
        for (const Pt& q : seam) moved.push_back({q.x - s * 1.6f, q.y});
        p.line(moved, WHITE, 1.0f, 26);
    }

    // przylbica wokol przycisku glownego: jasniejsza i wpuszczona
    std::vector<std::array<float, 3>> corners;
    for (const auto& q : {ph(450, 192), ph(830, 192), ph(765, 288), ph(515, 288)})
        corners.push_back({CX + q.x, CY + q.y, 14.0f});
    Shape visor = Shape::poly(rounded_poly(corners, 6));
    p.gradient(visor, VISOR_TOP, VISOR_BOTTOM);
    p.stroke(visor, BLACK, 1.2f, 150);

    // krawedz korpusu: ciemny obrys i jasny blask na gorze
    p.stroke(body, BLACK, 1.4f, 220);
    std::vector<Pt> edge_top;
    for (const Pt& q : body_outline())
        if (q.y < -120) edge_top.push_back({CX + q.x, CY + q.y + 1.3f});
    std::sort(edge_top.begin(), edge_top.end(), [](const Pt& a, const Pt& b) { return a.x < b.x; });
    p.line(edge_top, WHITE, 1.1f, 60);

    // gniazdo krzyzaka: podzialka dookola talerza
    float dx = CX + DPAD_C.x, dy = CY + DPAD_C.y;
    p.shadow(Shape::circle(dx, dy, DPAD_R), 4, 4, 200);
    for (int k = 0; k < 16; ++k) {
        float a = rad(static_cast<float>(k) * 22.5f);
        float r0 = DPAD_R + 4.5f, r1 = DPAD_R + (k % 2 == 0 ? 9.0f : 7.0f);
        p.line({{dx + r0 * std::cos(a), dy + r0 * std::sin(a)}, {dx + r1 * std::cos(a), dy + r1 * std::sin(a)}}, 0x5c5e63, 1.1f, 200, false);
    }
    p.gradient(Shape::circle(dx, dy, DPAD_HUB), 0x9a9ca0, 0x55575b);
    p.stroke(Shape::circle(dx, dy, DPAD_HUB), 0x2a2b2e, 1.0f);

    // ozdoby: Share i diody profilu
    Shape share = Shape::rrect(CX + SHARE_C.x - SHARE_HW, CY + SHARE_C.y - SHARE_HH, CX + SHARE_C.x + SHARE_HW, CY + SHARE_C.y + SHARE_HH, SHARE_HH);
    p.shadow(share, 1.5f, 2, 170);
    p.gradient(share, 0x2e3034, 0x0a0b0c);
    p.stroke(share, BLACK, 1.0f);
    p.sheen(share, 40, 0.5f);
    for (const Pt& led : {ph(640, 492), ph(640, 508), ph(640, 524)}) {
        Shape bar = Shape::rrect(CX + led.x - LED_HALF, CY + led.y - 1.6f, CX + led.x + LED_HALF, CY + led.y + 1.6f, 1.6f);
        p.glow(bar, WHITE, 3, 90);
        p.fill(bar, 0xf2f3f5);
    }

    // cienie drazkow w gniazdach - padaja w dol
    for (const Pt& s : {STICK_L, STICK_R}) p.shadow(Shape::circle(CX + s.x, CY + s.y, STICK_RING), 2, 3, 160);
}

// --------------------------------------------------------------- nazwy gniazd

const LabelMap& pad_row_labels() {
    static const LabelMap labels = [] {
        LabelMap m = {{"LT", "LT (lewy)"}, {"RT", "RT (prawy)"}, {"LB", "LB"}, {"RB", "RB"}, {"LS", "L3"}, {"RS", "R3"},
                      {"A", "A"}, {"B", "B"}, {"X", "X"}, {"Y", "Y"}, {"BACK", "Back"}, {"START", "Start"}, {"GUIDE", "Guide"},
                      {"DPAD_UP", "▲ góra"}, {"DPAD_DOWN", "▼ dół"}, {"DPAD_LEFT", "◀ lewo"}, {"DPAD_RIGHT", "▶ prawo"}};
        const std::map<std::string, std::string> arrows = {{"LX-", "◀"}, {"LX+", "▶"}, {"LY+", "▲"}, {"LY-", "▼"},
                                                           {"RX-", "◀"}, {"RX+", "▶"}, {"RY+", "▲"}, {"RY-", "▼"}};
        for (const auto& [d, arrow] : arrows) m[d] = arrow + " " + PAD_DIRECTION_SHORT.at(d);
        return m;
    }();
    return labels;
}

constexpr const char* EMPTY_SLOT = "—";  // pauza - gniazdo bez przypisania

} // namespace

// -------------------------------------------------------------- PadCanvas

PadCanvas::PadCanvas(int width, int height, Names controls, Names axis_parts, std::string id)
    : w_(width), h_(height), controls_(std::move(controls)), axis_parts_(std::move(axis_parts)), id_(std::move(id)) {}

Part& PadCanvas::add(const std::string& name, Shape shape, const CapStyle& style, Glyph glyph, bool clickable, bool shadow) {
    Part part;
    part.name = name;
    part.shape = std::move(shape);
    part.style = style;
    part.glyph = std::move(glyph);
    part.clickable = clickable;
    part.shadow = shadow;
    parts_.push_back(std::move(part));
    return parts_.back();
}

void PadCanvas::paint_body(Paint& p) { paint_pad_body(p); }

void PadCanvas::build_parts() {
    CapStyle gloss = gloss_style();
    CapStyle small_style = gloss;
    small_style.glyph = 0xc4c6ca;
    CapStyle guide = assigned_edge(with(gloss, 0xeeeef0, 0xb4b6ba, 0x5d5f63, 0x2a2b2e, true), 0x5d5f63);
    CapStyle metal = assigned_edge(with(gloss, 0x74767b, 0x3a3c40, 0x202124, 0xe6e7ea, false), 0x202124);
    CapStyle bumper = with(gloss, 0x62656b, 0x232428, 0x000000, 0xd4d6da, false);
    CapStyle trigger = metal;
    trigger.top = 0x45474c; trigger.bottom = 0x18191b; trigger.edge = 0x050506;
    CapStyle well = assigned_edge(with(gloss, RECESS, 0x222327, 0x2f3034, 0x3a3c40, false), 0x2f3034);
    CapStyle cap = assigned_edge(with(gloss, 0x45474c, 0x141517, 0x050506, 0x000000, false), 0x050506);

    // spusty: garby wyrastajace zza bumperow
    for (const char* name : {"LT", "RT"}) {
        int sign = std::string(name) == "LT" ? -1 : 1;
        Shape shape = offset_poly(trigger_outline(sign), CX, CY);
        Glyph glyph = trigger_glyph(sign, shape);
        add(name, std::move(shape), trigger, std::move(glyph), false, false);
    }
    // bumpery: blyszczace polksiezyce wzdluz barkow
    for (const char* name : {"LB", "RB"}) {
        int sign = std::string(name) == "LB" ? -1 : 1;
        add(name, offset_poly(bumper_outline(sign), CX, CY), bumper, bumper_glyph(sign, name), true, true);
    }
    // drazki: gniazdo z rantem (klik: zrodla) i gumowa czasza (klik: L3/R3)
    add("STICK_L", Shape::circle(CX + STICK_L.x, CY + STICK_L.y, STICK_RING), well, stick_ticks(CX + STICK_L.x, CY + STICK_L.y), false, false);
    add("STICK_R", Shape::circle(CX + STICK_R.x, CY + STICK_R.y, STICK_RING), well, stick_ticks(CX + STICK_R.x, CY + STICK_R.y), false, false);
    add("LS", Shape::circle(CX + STICK_L.x, CY + STICK_L.y, STICK_KNOB), cap, knob_glyph(CX + STICK_L.x, CY + STICK_L.y));
    add("RS", Shape::circle(CX + STICK_R.x, CY + STICK_R.y, STICK_KNOB), cap, knob_glyph(CX + STICK_R.x, CY + STICK_R.y));

    // krzyzak: metalowy talerz z czterema fasetami (kazda swieci osobno)
    float dx = CX + DPAD_C.x, dy = CY + DPAD_C.y;
    struct Facet { const char* name; float a0, a1; RGB top, bottom; };
    const Facet facets[] = {{"DPAD_UP", -135, -45, 0xd2d3d6, 0x8d8f93},
                            {"DPAD_RIGHT", -45, 45, 0x83858a, 0x4f5155},
                            {"DPAD_DOWN", 45, 135, 0x64666a, 0x38393d},
                            {"DPAD_LEFT", 135, 225, 0xa9abaf, 0x6c6e72}};
    for (const Facet& f : facets) {
        CapStyle style = assigned_edge(metal, 0x1f2023);
        style.top = f.top; style.bottom = f.bottom; style.edge = 0x1f2023;
        add(f.name, Shape::ring(dx, dy, DPAD_R, DPAD_HUB, f.a0, f.a1), style, facet_glyph(dx, dy, f.a0, f.a1), true, false);
    }

    // przyciski A B X Y: blyszczaca czern z wytloczona litera
    float fx = CX + FACE_C.x, fy = CY + FACE_C.y, off = FACE_OFFSET;
    // (litery w kolorach z listy przypisan - latwiej trafic wzrokiem)
    struct Face { const char* name; float ox, oy; RGB color; };
    for (const Face& f : {Face{"Y", 0, -off, 0xf0c419}, Face{"X", -off, 0, 0x3f9be8}, Face{"B", off, 0, 0xe8534a}, Face{"A", 0, off, 0x5ac25a}}) {
        float x = fx + f.ox, y = fy + f.oy;
        CapStyle style = gloss;
        style.glyph = lighten(f.color, 0.12);
        add(f.name, Shape::circle(x, y, FACE_R), style, text_glyph(x, y + 0.5f, f.name, 12, true));
    }

    // View i Menu: male blyszczace przyciski z jasnymi ikonami
    float bx = CX - std::abs(VIEW_C.x), sx = CX + std::abs(VIEW_C.x), vy = CY + VIEW_C.y;
    add("BACK", Shape::circle(bx, vy, SMALL_R), small_style, view_glyph(bx, vy));
    add("START", Shape::circle(sx, vy, SMALL_R), small_style, menu_glyph(sx, vy));

    // przycisk glowny: jasny, blyszczacy (bez cudzego logo)
    float gx = CX + GUIDE_C.x, gy = CY + GUIDE_C.y;
    add("GUIDE", Shape::circle(gx, gy, GUIDE_R), guide, guide_glyph(gx, gy));
}

std::string PadCanvas::state_of(const std::string& name) const {
    if (learning_ && *learning_ == name) return pulse_on_ ? "learn_on" : "learn_off";
    auto a = active_.find(name);
    if (a != active_.end() && a->second) return "active";
    if (highlight_ && *highlight_ == name) return "hover";
    auto s = assigned_.find(name);
    if (s != assigned_.end() && s->second) return "assigned";
    return "idle";
}

const Sprite& PadCanvas::sprite(const Part& part, const std::string& state) {
    return Gfx::get().cached(id_ + "/" + part.name + "/" + state, [&] { return render_cap(part.shape, state, part.style, part.glyph, part.shadow); });
}

void PadCanvas::paint(Gfx& g) {
    const Sprite& back = g.cached(id_ + "/body", [&] {
        Paint p(w_, h_);
        paint_body(p);
        return p.finish();
    });
    g.draw_sprite(back, r.x, r.y);
    for (const Part& part : parts_) {
        g.draw_sprite(sprite(part, state_of(part.name)), r.x + part.dx, r.y + part.dy);
        paint_after_part(g, part);
    }
    paint_overlay(g);
}

void PadCanvas::paint_overlay(Gfx& g) {
    // napisy spustow - nad wszystkim
    for (const char* name : {"LT", "RT"}) {
        int raw = std::string(name) == "LT" ? lt_ : rt_;
        Pt l = trigger_label(std::string(name) == "LT" ? -1 : 1);
        g.text_at(r.x + CX + l.x, r.y + CY + l.y, raw == 0 ? std::string(name) : std::format("{}  {}", name, raw), Font::SmallBold, 0xeef0f3);
    }
}

std::vector<std::string> PadCanvas::parts_at(float x, float y) const {
    std::vector<std::string> out;
    for (auto it = parts_.rbegin(); it != parts_.rend(); ++it)
        if (it->shape.contains(x - it->dx, y - it->dy)) out.push_back(it->name);
    return out;
}

bool PadCanvas::hand_cursor() const {
    if (!hover_) return false;
    return contains(controls_, *hover_) || (contains(axis_parts_, *hover_) && handlers_.axis_click);
}

void PadCanvas::mouse_move(float x, float y) {
    auto names = parts_at(x - r.x, y - r.y);
    std::optional<std::string> name;
    if (!names.empty()) name = names[0];
    if (name == hover_) return;
    hover_ = name;
    if (handlers_.hover) handlers_.hover(name ? &*name : nullptr);
}

void PadCanvas::mouse_leave() {
    hover_.reset();
    if (handlers_.hover) handlers_.hover(nullptr);
}

void PadCanvas::mouse_down(float x, float y) {
    auto names = parts_at(x - r.x, y - r.y);
    std::string name = names.empty() ? "" : names[0];
    if (contains(controls_, name)) {
        if (handlers_.button_click) handlers_.button_click(name);
    } else if (contains(axis_parts_, name) && handlers_.axis_click) {
        handlers_.axis_click(name, x, y);
    } else if (handlers_.button_click) {
        handlers_.button_click("");  // klik w tlo anuluje nauke
    }
}

void PadCanvas::mouse_rdown(float x, float y) {
    auto names = parts_at(x - r.x, y - r.y);
    if (names.empty()) return;
    const std::string& name = names[0];
    if (contains(controls_, name)) {
        if (handlers_.button_clear) handlers_.button_clear(name);
    } else if (contains(axis_parts_, name) && handlers_.axis_clear) {
        handlers_.axis_clear(name);
    }
}

void PadCanvas::set_learning(const std::optional<std::string>& name) {
    if (learning_ != name) {
        learning_ = name;
        invalidate();
    }
}

void PadCanvas::set_pulse(bool on) {
    if (pulse_on_ != on) {
        pulse_on_ = on;
        if (learning_) invalidate();
    }
}

void PadCanvas::set_assigned(const std::map<std::string, bool>& assigned) {
    if (assigned_ != assigned) {
        assigned_ = assigned;
        invalidate();
    }
}

void PadCanvas::set_active(const std::map<std::string, bool>& active) {
    if (active_ != active) {
        active_ = active;
        invalidate();
    }
}

void PadCanvas::highlight(const std::optional<std::string>& part) {
    if (highlight_ != part) {
        highlight_ = part;
        invalidate();
    }
}

void PadCanvas::set_sticks(double lx, double ly, double rx, double ry) {
    auto clamp = [](double v) { return static_cast<float>(std::max(-1.0, std::min(1.0, v))); };
    for (Part& part : parts_) {
        double dx = 0, dy = 0;
        if (part.name == "LS") { dx = lx; dy = ly; }
        else if (part.name == "RS") { dx = rx; dy = ry; }
        else continue;
        float nx = clamp(dx) * STICK_TRAVEL, ny = -clamp(dy) * STICK_TRAVEL;
        if (std::abs(nx - part.dx) > 0.02f || std::abs(ny - part.dy) > 0.02f) {
            part.dx = nx;
            part.dy = ny;
            invalidate();
        }
    }
}

void PadCanvas::set_triggers(int lt, int rt) {
    lt = std::clamp(lt, 0, 255);
    rt = std::clamp(rt, 0, 255);
    if (lt != lt_ || rt != rt_) {
        lt_ = lt;
        rt_ = rt;
        invalidate();
    }
}

XboxCanvas::XboxCanvas() : PadCanvas(PAD_W, PAD_H, PAD_BUTTON_NAMES, {"LT", "RT", "STICK_L", "STICK_R"}, "pad") {}

void XboxCanvas::paint_after_part(Gfx& g, const Part& part) {
    // wypelnienie spustu nad jego obrazkiem, pod bumperem
    if (part.name != "LT" && part.name != "RT") return;
    int raw = part.name == "LT" ? lt_ : rt_;
    int step = static_cast<int>(std::lrint(raw / 255.0 * TRIGGER_STEPS));
    if (step == 0) return;
    int sign = part.name == "LT" ? -1 : 1;
    const Sprite& s = g.cached(std::format("pad/fill/{}/{}", part.name, step), [&] { return render_trigger_fill(part.shape, sign, step); });
    g.draw_sprite(s, r.x, r.y);
}

// ----------------------------------------------------------- rysunek G29

namespace g29 {

constexpr int W = 486, H = 388;
constexpr float WCX = 243.0f, WCY = 178.0f;
constexpr float RIM_OUT = 150.0f, RIM_IN = 121.0f;
constexpr float PLATE[5] = {-125, -50, 125, 54, 24};
constexpr float SPOKE[5] = {-50, 30, 50, 128, 18};
constexpr float PADDLE[4] = {-206, -86, -158, -24};
constexpr Pt DPAD{-82, -12};
constexpr float DPAD_ARM = 22, DPAD_HALF = 7.5f, DPAD_GAP = 8;
constexpr Pt FACE{82, -12};
constexpr float FACE_STEP = 20, G_FACE_R = 9.5f, G_SMALL_R = 10;
constexpr float PILL_W = 22, PILL_H = 10;
constexpr Pt PS{0, 20};
constexpr float PS_R = 11;
constexpr Pt PLUS{-26, 66}, MINUS{-26, 92};
constexpr float PM_R = 8;
constexpr Pt DIAL{16, 80};
constexpr float DIAL_OUT = 22, DIAL_IN = 12, ENTER_R = 9, GEAR_R = 9;
constexpr float PEDAL_TOP = 126, PEDAL_BOTTOM = 186, PEDAL_HALF = 7;

struct Gear { const char* name; float x, y; const char* label; };
constexpr Gear GEARS[] = {{"GEAR_1", 152, 140, "1"}, {"GEAR_3", 176, 140, "3"}, {"GEAR_5", 200, 140, "5"}, {"GEAR_R", 224, 140, "R"},
                          {"GEAR_2", 152, 188, "2"}, {"GEAR_4", 176, 188, "4"}, {"GEAR_6", 200, 188, "6"}};
struct PedalSpot { const char* key; float x; const char* text; RGB color; };
constexpr PedalSpot PEDALS[] = {{"clutch", -222, "SPR", WARN}, {"brake", -196, "HAM", BAD}, {"throttle", -170, "GAZ", GOOD}};

CapStyle cap_style() {
    CapStyle s{};
    s.top = 0x3c4555; s.bottom = 0x232a34; s.edge = 0x566279; s.on = ACCENT; s.glyph = TEXT_DIM; s.glyph_on = ACCENT_INK;
    s.accent = ACCENT; s.warn = WARN; s.ink = 0x20160a;
    return s;
}

Glyph groove_glyph(float x, float y1, float y2) {
    return [=](Paint& p, RGB color) { p.line({{x, y1}, {x, y2}}, color, 2.2f, 150); };
}

Glyph dot_glyph(float x, float y, float rr) {
    return [=](Paint& p, RGB color) { p.fill(Shape::circle(x, y, rr), color); };
}

Glyph face_glyph(const std::string& name, float x, float y) {
    return [=](Paint& p, RGB color) {
        if (name == "TRIANGLE") {
            Pt a{x, y - 5.2f}, b{x + 5.2f, y + 3.8f}, c{x - 5.2f, y + 3.8f};
            p.line({a, b, c, a}, color, 1.5f);
        } else if (name == "SQUARE") {
            p.stroke(Shape::rrect(x - 4.3f, y - 4.3f, x + 4.3f, y + 4.3f, 0.8f), color, 1.5f);
        } else if (name == "CIRCLE") {
            p.stroke(Shape::circle(x, y, 4.8f), color, 1.5f);
        } else {
            p.line({{x - 4.2f, y - 4.2f}, {x + 4.2f, y + 4.2f}}, color, 1.5f);
            p.line({{x - 4.2f, y + 4.2f}, {x + 4.2f, y - 4.2f}}, color, 1.5f);
        }
    };
}

} // namespace g29

WheelCanvas::WheelCanvas() : PadCanvas(g29::W, g29::H, G29_CONTROLS, {}, "g29") {}

void WheelCanvas::paint_body(Paint& p) {
    using namespace g29;
    // trzonki lopatek - chowaja sie za obrecza
    for (float side : {-1.0f, 1.0f}) {
        float lo = std::min(WCX + side * 138.0f, WCX + side * 166.0f), hi = std::max(WCX + side * 138.0f, WCX + side * 166.0f);
        p.gradient(Shape::rrect(lo, WCY - 62, hi, WCY - 48, 3), 0x4a5465, 0x2b323e);
    }

    // obrecz z gumy: cien, gradient, polysk i przeszycie
    Shape rim = Shape::ring(WCX, WCY, RIM_OUT, RIM_IN, -90, 270);
    p.shadow(rim, 10, 10, 170);
    p.gradient(rim, 0x424a59, 0x14181e);
    // uchwyty po bokach: ciemniejsza, perforowana skora z przeszyciem na koncach
    for (float ctr : {0.0f, 180.0f}) {
        Shape grip = Shape::ring(WCX, WCY, RIM_OUT - 1, RIM_IN + 1, ctr - 40, ctr + 40);
        p.gradient(grip, 0x2a303a, 0x0f1217);
        int row = 0;
        for (float rr = RIM_IN + 5; rr <= RIM_OUT - 4.5f; rr += 4.0f, ++row)
            for (float deg = ctr - 37 + (row % 2 ? 1.2f : 0.0f); deg <= ctr + 37; deg += 2.4f)
                p.fill(Shape::circle(WCX + rr * std::cos(rad(deg)), WCY + rr * std::sin(rad(deg)), 0.8f), BLACK, 130);
        for (float deg : {ctr - 40, ctr + 40})
            p.line({{WCX + (RIM_IN + 2) * std::cos(rad(deg)), WCY + (RIM_IN + 2) * std::sin(rad(deg))},
                    {WCX + (RIM_OUT - 2) * std::cos(rad(deg)), WCY + (RIM_OUT - 2) * std::sin(rad(deg))}}, 0x9aa5b8, 1.3f, 150, false);
    }
    p.sheen(rim, 46, 0.35f);
    float mid = (RIM_OUT + RIM_IN) / 2;
    for (int k = 0; k < 120; ++k) {
        float deg = static_cast<float>(k) * 3.0f;
        if (std::fmod(deg + 42.0f, 180.0f) < 84.0f) continue;  // pod uchwytem szwu nie ma
        float a = rad(deg), b = rad(deg + 1.4f);
        p.line({{WCX + mid * std::cos(a), WCY + mid * std::sin(a)}, {WCX + mid * std::cos(b), WCY + mid * std::sin(b)}}, WHITE, 0.9f, 40);
    }
    p.stroke(Shape::circle(WCX, WCY, RIM_OUT), WHITE, 1.0f, 34);
    p.arc(WCX, WCY, RIM_IN + 0.9f, 0, 360, BLACK, 1.8f, 120);

    // plyta z przyciskami i dolne ramie - jedna bryla ze szczotkowanego aluminium
    Shape plate = Shape::rrect(WCX + PLATE[0], WCY + PLATE[1], WCX + PLATE[2], WCY + PLATE[3], PLATE[4]);
    Shape spoke = Shape::rrect(WCX + SPOKE[0], WCY + SPOKE[1], WCX + SPOKE[2], WCY + SPOKE[3], SPOKE[4]);
    for (const Shape* s : {&spoke, &plate}) p.shadow(*s, 6, 7, 170);
    float top = WCY + PLATE[1], bottom = WCY + SPOKE[3];
    for (const Shape* s : {&spoke, &plate}) {
        p.gradient(*s, 0x515c70, 0x242a34, top, bottom);
        p.push_clip(*s);
        for (float y = top + 1; y < bottom; y += 2.5f) p.line({{WCX - 130, y}, {WCX + 130, y}}, WHITE, 0.5f, 10, false);
        p.pop_clip();
    }
    p.sheen(plate, 38, 0.5f);
    p.stroke(plate, WHITE, 1.0f, 34);
    p.stroke(plate.grow(-4), BLACK, 1.0f, 50);
    // sruby w rogach plyty
    for (float sx : {-113.0f, 113.0f})
        for (float sy : {-39.0f, 43.0f}) {
            Shape screw = Shape::circle(WCX + sx, WCY + sy, 2.8f);
            p.gradient(screw, 0x8a94a6, 0x2b313b);
            p.stroke(screw, BLACK, 0.8f, 160);
            p.line({{WCX + sx - 1.6f, WCY + sy - 1.0f}, {WCX + sx + 1.6f, WCY + sy + 1.0f}}, BLACK, 0.9f, 170, false);
        }
    p.text(WCX, WCY - 10, "G29", 9, 0x8a95a8, true);

    // wglebienia pod krzyzakiem i pokretlem
    float dx = WCX + DPAD.x, dy = WCY + DPAD.y;
    p.gradient(Shape::circle(dx, dy, DPAD_ARM + 6), 0x151a21, 0x2a313c);
    p.fill(Shape::circle(dx, dy, DPAD_GAP - 1), 0x1c222b);
    p.fill(Shape::circle(WCX + DIAL.x, WCY + DIAL.y, DIAL_OUT + 3), 0x14181f);

    // skrzynia biegow: plyta i prowadnica H
    float xs[4] = {152, 176, 200, 224};
    float top_y = 140, bottom_y = 188;
    Shape gate = Shape::rrect(WCX + xs[0] - 18, WCY + top_y - 18, WCX + xs[3] + 18, WCY + bottom_y + 18, 14);
    p.shadow(gate, 4, 5, 150);
    p.gradient(gate, 0x2e3541, 0x1b2028);
    p.stroke(gate, WHITE, 1.0f, 24);
    float mid_y = (top_y + bottom_y) / 2;
    const RGB slot = 0x0e1116;
    p.line({{WCX + xs[0], WCY + mid_y}, {WCX + xs[3], WCY + mid_y}}, slot, 4);
    for (float gx : xs) {
        bool has_bottom = false;
        for (const Gear& gear : GEARS)
            if (gear.x == gx && gear.y == bottom_y) has_bottom = true;
        p.line({{WCX + gx, WCY + top_y}, {WCX + gx, WCY + (has_bottom ? bottom_y : mid_y)}}, slot, 4);
    }

    // pedaly: rowki pod paskami
    for (const PedalSpot& spot : PEDALS) {
        Shape track = Shape::rrect(WCX + spot.x - PEDAL_HALF, WCY + PEDAL_TOP, WCX + spot.x + PEDAL_HALF, WCY + PEDAL_BOTTOM, PEDAL_HALF);
        p.gradient(track, 0x0e1116, 0x1d232c);
        p.stroke(track, WHITE, 1.0f, 20);
    }
}

void WheelCanvas::build_parts() {
    using namespace g29;
    CapStyle cap = cap_style();
    CapStyle metal = with(cap, 0x7d889c, 0x4a5465, 0x98a3b6, 0x2a313c, true);
    CapStyle red = with(cap, 0xc94850, 0x8e252d, 0xd9636b, 0xf6d4d7, true);
    CapStyle red_dark = red;
    red_dark.top = 0x8a2730; red_dark.bottom = 0x5e1820; red_dark.edge = 0xa8323a;

    // lopatki - PADDLE opisuje lewa, prawa to jej odbicie
    float ax = std::abs(PADDLE[0]), bx = std::abs(PADDLE[2]);
    for (auto [name, side] : {std::pair{"PADDLE_L", -1.0f}, std::pair{"PADDLE_R", 1.0f}}) {
        float lo = std::min(side * ax, side * bx), hi = std::max(side * ax, side * bx);
        float cx = WCX + (lo + hi) / 2;
        add(name, Shape::rrect(WCX + lo, WCY + PADDLE[1], WCX + hi, WCY + PADDLE[3], 12), metal, groove_glyph(cx, WCY + PADDLE[1] + 13, WCY + PADDLE[3] - 13));
    }

    // krzyzak
    float cx = WCX + DPAD.x, cy = WCY + DPAD.y, a = DPAD_ARM, h = DPAD_HALF, gap = DPAD_GAP;
    struct Arm { const char* name; float x1, y1, x2, y2; int ux, uy; };
    for (const Arm& arm : {Arm{"DPAD_UP", cx - h, cy - a, cx + h, cy - gap, 0, -1}, Arm{"DPAD_DOWN", cx - h, cy + gap, cx + h, cy + a, 0, 1},
                           Arm{"DPAD_LEFT", cx - a, cy - h, cx - gap, cy + h, -1, 0}, Arm{"DPAD_RIGHT", cx + gap, cy - h, cx + a, cy + h, 1, 0}}) {
        float mx = (arm.x1 + arm.x2) / 2, my = (arm.y1 + arm.y2) / 2;
        add(arm.name, Shape::rrect(arm.x1, arm.y1, arm.x2, arm.y2, 3.5f), cap, arrow_glyph(mx, my, arm.ux, arm.uy, 3.2f));
    }

    // przyciski twarzowe z piktogramami
    float fx = WCX + FACE.x, fy = WCY + FACE.y, s = FACE_STEP;
    struct FaceSpot { const char* name; float x, y; RGB color; };
    for (const FaceSpot& f : {FaceSpot{"TRIANGLE", fx, fy - s, 0x3fbf9f}, FaceSpot{"SQUARE", fx - s, fy, 0xd97ab8},
                              FaceSpot{"CIRCLE", fx + s, fy, 0xe8534a}, FaceSpot{"CROSS", fx, fy + s, 0x5b8def}}) {
        CapStyle style = with(cap, 0x2f3642, 0x1c2129, mix(0x3e4859, f.color, 0.4), f.color, true);
        style.on = f.color;
        style.glyph_on = INK;
        add(f.name, Shape::circle(f.x, f.y, G_FACE_R), style, face_glyph(f.name, f.x, f.y));
    }

    // L2/L3/R2/R3
    struct SmallSpot { const char* name; float x, y; };
    for (const SmallSpot& sp : {SmallSpot{"L2", -104, 28}, SmallSpot{"L3", -74, 36}, SmallSpot{"R2", 104, 28}, SmallSpot{"R3", 74, 36}})
        add(sp.name, Shape::circle(WCX + sp.x, WCY + sp.y, G_SMALL_R), cap, text_glyph(WCX + sp.x, WCY + sp.y, sp.name, 6.5f, true));

    // Share / Options / PS
    float w = PILL_W / 2, ph_ = PILL_H / 2;
    add("SHARE", Shape::rrect(WCX - 34 - w, WCY - 27 - ph_, WCX - 34 + w, WCY - 27 + ph_, ph_), cap);
    add("OPTIONS", Shape::rrect(WCX + 34 - w, WCY - 27 - ph_, WCX + 34 + w, WCY - 27 + ph_, ph_), cap);
    CapStyle ps = cap;
    ps.top = 0x2a313c; ps.bottom = 0x171b22;
    add("PS", Shape::circle(WCX + PS.x, WCY + PS.y, PS_R), ps, text_glyph(WCX + PS.x, WCY + PS.y, "PS", 7, true));

    // plus, minus, pokretlo i Enter
    add("PLUS", Shape::circle(WCX + PLUS.x, WCY + PLUS.y, PM_R), cap, text_glyph(WCX + PLUS.x, WCY + PLUS.y - 0.5f, "+", 9, true));
    add("MINUS", Shape::circle(WCX + MINUS.x, WCY + MINUS.y, PM_R), cap, text_glyph(WCX + MINUS.x, WCY + MINUS.y - 0.5f, "−", 9, true));
    float x = WCX + DIAL.x, y = WCY + DIAL.y;
    // dwie polowki pokretla; 4 stopnie przerwy - to dwa osobne gniazda
    add("DIAL_CW", Shape::ring(x, y, DIAL_OUT, DIAL_IN, -88, 88), red, text_glyph(x + 17, y - 0.5f, "›", 9, true));
    add("DIAL_CCW", Shape::ring(x, y, DIAL_OUT, DIAL_IN, 92, 268), red, text_glyph(x - 17, y - 0.5f, "‹", 9, true));
    add("ENTER", Shape::circle(x, y, ENTER_R), red_dark, dot_glyph(x, y, 2.6f));

    // biegi
    for (const Gear& gear : GEARS)
        add(gear.name, Shape::circle(WCX + gear.x, WCY + gear.y, GEAR_R), metal, text_glyph(WCX + gear.x, WCY + gear.y, gear.label, 7.5f, true));
}

void WheelCanvas::paint_overlay(Gfx& g) {
    using namespace g29;
    float ox = r.x, oy = r.y;
    float ax = std::abs(PADDLE[0]), bx = std::abs(PADDLE[2]);
    for (auto [text, side] : {std::pair{"L", -1.0f}, std::pair{"P", 1.0f}}) {
        float lo = std::min(side * ax, side * bx), hi = std::max(side * ax, side * bx);
        g.text_at(ox + WCX + (lo + hi) / 2, oy + WCY + PADDLE[3] + 13, text, Font::TinyBold, MUTED);
    }
    g.text_at(ox + WCX - 34, oy + WCY - 27 - PILL_H / 2 - 6, "SHARE", Font::Micro, MUTED);
    g.text_at(ox + WCX + 34, oy + WCY - 27 - PILL_H / 2 - 6, "OPTIONS", Font::Micro, MUTED);
    g.text_at(ox + WCX + (152.0f + 224.0f) / 2, oy + WCY + 140 - 30, "SKRZYNIA", Font::TinyBold, MUTED);

    float h = PEDAL_HALF - 2;
    float span = PEDAL_BOTTOM - PEDAL_TOP - 6;
    for (const PedalSpot& spot : PEDALS) {
        double v = pedals_[static_cast<size_t>(pedal_index(spot.key))];
        v = v != v ? 0.0 : std::max(0.0, std::min(1.0, v));
        float len = static_cast<float>(v) * span;
        if (len >= 2) {
            Rect fill{ox + WCX + spot.x - h, oy + WCY + PEDAL_BOTTOM - 3 - len, 2 * h, len};
            g.shadow(fill, h, 6, spot.color, 0.30f);
            g.fill_rrect_v(fill, h, lighten(spot.color, 0.22), mix(spot.color, BG, 0.25));
        }
        g.text_at(ox + WCX + spot.x, oy + WCY + PEDAL_BOTTOM + 12, spot.text, Font::Micro, MUTED);
    }
    g.text_at(ox + WCX + PEDALS[1].x, oy + WCY + PEDAL_TOP - 16, "PEDAŁY", Font::TinyBold, MUTED);

    // znacznik obrotu na obreczy - kreska jak pasek na gorze prawdziwego kola
    float a = rad(-90.0f + static_cast<float>(angle_));
    float r1 = RIM_IN + 4, r2 = RIM_OUT - 4;
    float x1 = ox + WCX + r1 * std::cos(a), y1 = oy + WCY + r1 * std::sin(a);
    float x2 = ox + WCX + r2 * std::cos(a), y2 = oy + WCY + r2 * std::sin(a);
    g.polyline({{x1, y1}, {x2, y2}}, ACCENT, 9, 70.0f / 255.0f);
    g.polyline({{x1, y1}, {x2, y2}}, ACCENT, 5.5f);
    g.text_at(ox + WCX, oy + WCY - 84, std::format("{:+.0f}°", angle_), Font::Heading, TEXT);
}

void WheelCanvas::set_angle(double deg) {
    if (deg != deg) deg = 0;
    if (std::abs(deg - angle_) >= 0.05) {
        angle_ = deg;
        invalidate();
    }
}

void WheelCanvas::set_pedals(const std::array<double, NUM_PEDALS>& values) {
    if (values != pedals_) {
        pedals_ = values;
        invalidate();
    }
}

// ------------------------------------------------------------ lista wierszy

class PadView::Row final : public Widget {
public:
    static constexpr float BAR_W = 46;
    static constexpr float BAR_H = 8;

    Row(PadView* owner, std::string slot_name, bool is_axis, std::string label_text, std::optional<RGB> name_color, float name_w)
        : view(owner), slot(std::move(slot_name)), axis(is_axis), label(std::move(label_text)), color(name_color), name_width(name_w) {}

    PadView* view;
    std::string slot;
    bool axis;
    std::string label;
    std::optional<RGB> color;
    float name_width;

    std::string state = "empty";  // empty | set | active | learning
    std::string text;
    bool hover = false;
    bool pulse = true;
    double value = 0;
    double flash_until = 0;

    Size measure(float avail_w) override { return {avail_w, 31}; }

    void set_text(const std::string& t) {
        if (text != t) {
            text = t;
            invalidate();
        }
    }

    void flash() {
        flash_until = monotonic() + 0.9;
        invalidate();
    }

    void set_state(const std::string& s, bool h, bool p) {
        if (s != state || h != hover || p != pulse) {
            state = s;
            hover = h;
            pulse = p;
            invalidate();
        }
    }

    void set_value(double v) {
        v = v < 0 ? 0 : (v > 1 ? 1 : v);
        if (std::abs(v - value) >= 0.004) {
            value = v;
            invalidate();
        }
    }

    bool hand_cursor() const override { return true; }
    void mouse_enter() override { view->on_row_hover(&slot); }
    void mouse_leave() override { view->on_row_hover(nullptr); }
    void mouse_down(float x, float y) override { view->on_row_click(slot, x, y); }
    void mouse_rdown(float, float) override { view->on_row_clear(slot); }

    void paint(Gfx& g) override {
        RGB name_fg = color ? (hover ? lighten(*color, 0.25) : *color) : (hover ? TEXT : TEXT_DIM);
        g.text({r.x + 4, r.y, name_width - 8, r.h}, label, color ? Font::SmallBold : Font::Small, name_fg);

        float cell_x = r.x + name_width;
        float cell_w = r.w - name_width - (BAR_W + 10);
        Rect cell{cell_x, r.y + 2, std::max(40.0f, cell_w), r.h - 4};

        bool flashing = monotonic() < flash_until && state != "learning";
        RGB bg, fg, border;
        if (flashing) {
            bg = mix(PANEL_HI, GOOD, 0.30);
            fg = WHITE;
            border = GOOD;
        } else if (state == "learning") {
            bg = pulse ? WARN : mix(WARN, PANEL_HI, 0.55);
            fg = pulse ? 0x20160a : TEXT;
            border = WARN;
        } else if (state == "active") {
            bg = mix(PANEL_HI, ACCENT, 0.45);
            fg = WHITE;
            border = ACCENT;
        } else if (state == "set") {
            bg = PANEL_HI;
            fg = TEXT;
            border = mix(BORDER, ACCENT, 0.42);
        } else {
            bg = PANEL_HI;
            fg = MUTED;
            border = BORDER;
        }
        if (!flashing && hover && state != "learning" && state != "active") {
            bg = lighten(bg, 0.07);
            border = mix(border, ACCENT, 0.55);
        }
        g.fill_rrect(cell, 7, bg);
        g.stroke_rrect(cell, 7, border);
        std::string shown = state == "learning" ? "wciśnij…" : (text.empty() ? EMPTY_SLOT : text);
        g.text_fit({cell.x + 10, cell.y, cell.w - 14, cell.h}, shown, Font::Small, fg);

        if (axis) {
            float bx = cell.right() + 8, by = r.y + (r.h - BAR_H) / 2;
            g.fill_rrect({bx, by + 1, BAR_W, BAR_H - 2}, (BAR_H - 2) / 2, TRACK);
            if (value >= 0.02) g.fill_rrect({bx, by + 1, BAR_W * static_cast<float>(value), BAR_H - 2}, (BAR_H - 2) / 2, ACCENT);
        }
    }
};

// ------------------------------------------------------------------ PadView

PadView::PadView(PadViewSpec spec, Callbacks callbacks) : spec_(std::move(spec)), cb_(std::move(callbacks)) {
    canvas_ = std::move(spec_.canvas);
    canvas_->build();
    PadCanvas::Handlers h;
    h.button_click = [this](const std::string& n) { on_canvas_click(n); };
    h.button_clear = [this](const std::string& n) { if (cb_.clear) cb_.clear(n); };
    h.hover = [this](const std::string* p) { on_canvas_hover(p); };
    h.axis_click = spec_.axes.empty() ? nullptr : std::function<void(const std::string&, float, float)>([this](const std::string& p, float x, float y) { on_canvas_axis(p, x, y); });
    h.axis_clear = spec_.axes.empty() ? nullptr : std::function<void(const std::string&)>([this](const std::string& p) { on_canvas_axis_clear(p); });
    canvas_->set_handlers(std::move(h));

    for (const auto& n : spec_.controls) assign_[n] = -1;
    for (const auto& a : spec_.axes) axis_labels_[a] = "";

    for (const auto& column : spec_.groups)
        for (const auto& group : column) {
            headers_.push_back({upper_pl(group.title), {}});
            for (const auto& slot : group.slots) {
                auto label = spec_.row_labels.find(slot);
                auto color = spec_.row_colors.find(slot);
                auto row = std::make_unique<Row>(this, slot, group.axis, label == spec_.row_labels.end() ? slot : label->second,
                                                 color == spec_.row_colors.end() ? std::nullopt : std::optional<RGB>(color->second), spec_.name_minsize);
                row_of_[slot] = row.get();
                rows_.push_back(std::move(row));
            }
        }
    refresh_rows();
}

PadView::~PadView() = default;

std::string PadView::hint_text() const {
    if (!learning_) return spec_.idle_hint;
    std::vector<std::string> parts;
    if (wizard_) parts.push_back(std::format("Krok {} z {}", wizard_->first, wizard_->second));
    auto title = spec_.titles.find(*learning_);
    parts.push_back("Wciśnij przycisk kierownicy dla: " + (title == spec_.titles.end() ? *learning_ : title->second));
    auto cur = assign_.find(*learning_);
    if (cur != assign_.end() && cur->second >= 0) parts.push_back(std::format("teraz: przycisk {}", cur->second));
    if (!held_.empty()) {
        std::string held;
        for (int b : held_) held += (held.empty() ? "" : ", ") + std::to_string(b);
        parts.push_back("trzymasz: " + held);
    }
    parts.push_back(wizard_ ? "Esc kończy" : "Esc anuluje");
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : "      ") + p;
    return out;
}

float PadView::hint_height(float w) { return Gfx::get().measure(hint_text(), Font::Small, std::max(120.0f, w - 52)).height + 20; }

float PadView::list_min_height(float) {
    Gfx& g = Gfx::get();
    float row_h = 31;
    float head_h = g.measure("Ag", Font::Caps).height + 6;
    float best = 0;
    for (const auto& column : spec_.groups) {
        float h = 0;
        for (size_t i = 0; i < column.size(); ++i) {
            h += head_h + (i ? 11 : 0);
            h += row_h * static_cast<float>(column[i].slots.size());
        }
        best = std::max(best, h);
    }
    return best;
}

Size PadView::measure(float avail_w) {
    Size canvas = canvas_->measure(avail_w);
    float legend_h = Gfx::get().measure("Ag", Font::Tiny).height + 16;
    float hint = hint_height(avail_w) + 14;
    bool stacked = avail_w < canvas.w + 28 + 470;
    float left = canvas.h + legend_h;
    float list = list_min_height(avail_w);
    return {avail_w, hint + (stacked ? left + 16 + list : std::max(left, list))};
}

void PadView::arrange(const Rect& rc) {
    r = rc;
    Gfx& g = Gfx::get();
    float hint_h = hint_height(rc.w);
    hint_r_ = {rc.x, rc.y, rc.w, hint_h};
    Size canvas = canvas_->measure(rc.w);
    float legend_h = g.measure("Ag", Font::Tiny).height + 16;
    bool stacked = rc.w < canvas.w + 28 + 470;
    float top = rc.y + hint_h + 14;

    float cx = stacked ? rc.x + (rc.w - canvas.w) / 2 : rc.x;
    canvas_->arrange({cx, top, canvas.w, canvas.h});
    legend_r_ = {cx, top + canvas.h, canvas.w, legend_h};

    stage_r_ = stacked ? Rect{rc.x, top, rc.w, canvas.h + legend_h} : Rect{rc.x, top, canvas.w, canvas.h + legend_h};
    float list_x = stacked ? rc.x : rc.x + canvas.w + 28;
    float list_y = stacked ? top + canvas.h + legend_h + 16 : top;
    float list_w = stacked ? rc.w : rc.w - canvas.w - 28;

    size_t col_count = spec_.groups.size();
    float gap = 22;
    float col_w = (list_w - gap * static_cast<float>(col_count - 1)) / static_cast<float>(col_count);
    float row_h = 31;
    float head_h = g.measure("Ag", Font::Caps).height + 6;

    size_t header = 0;
    for (size_t c = 0; c < col_count; ++c) {
        float x = list_x + static_cast<float>(c) * (col_w + gap);
        float y = list_y;
        for (size_t i = 0; i < spec_.groups[c].size(); ++i) {
            if (i) y += 11;
            headers_[header++].r = {x, y, col_w, head_h};
            y += head_h;
            for (const auto& slot : spec_.groups[c][i].slots) {
                row_of_[slot]->arrange({x, y, col_w, row_h});
                y += row_h;
            }
        }
    }
}

void PadView::paint(Gfx& g) {
    bool learning = learning_.has_value();
    RGB bg = learning ? mix(PANEL_HI, WARN, 0.14) : PANEL_HI;
    RGB border = learning ? mix(BORDER, WARN, 0.5) : BORDER;
    g.fill_rrect(hint_r_, 10, bg);
    g.stroke_rrect(hint_r_, 10, border);
    g.text({hint_r_.x + 12, hint_r_.y, 20, hint_r_.h}, learning ? ic::target : ic::info, Font::IconSmall, learning ? WARN : MUTED, Align::Center);
    g.text({hint_r_.x + 40, hint_r_.y, hint_r_.w - 52, hint_r_.h}, hint_text(), Font::Small, learning ? WARN : TEXT_DIM, Align::Left, true);
    if (wizard_ && wizard_->second > 0) {
        // postep kreatora na dolnej krawedzi podpowiedzi
        float w = (hint_r_.w - 24) * static_cast<float>(wizard_->first) / static_cast<float>(wizard_->second);
        g.fill_rrect({hint_r_.x + 12, hint_r_.bottom() - 4, hint_r_.w - 24, 2}, 1, BORDER_HI);
        g.fill_rrect({hint_r_.x + 12, hint_r_.bottom() - 4, w, 2}, 1, WARN);
    }

    // scena: miekkie swiatlo pod rysunkiem
    Rect cv = canvas_->r;
    g.fill_radial(stage_r_, 14, cv.x + cv.w / 2, cv.y + cv.h * 0.45f, std::max(cv.w, cv.h) * 0.62f, 0x2a3442, 0x151a23, 0.85f, 0.0f);
    canvas_->paint(g);

    // legenda kolorow - bez niej trzeba zgadywac, co znaczy podswietlenie
    struct Entry { const char* text; RGB color; };
    const Entry items[] = {{"wciśnięte", ACCENT}, {"przypisane", mix(PANEL_HI, ACCENT, 0.20)}, {"uczy się", WARN}, {"wolne", PANEL_HI}};
    float total = 0;
    for (const Entry& e : items) total += 16 + g.measure(e.text, Font::Tiny).width + 18;
    float x = cv.x + (cv.w - total + 18) / 2, y = cv.bottom() + 8;
    float th = g.measure("Ag", Font::Tiny).height;
    for (const Entry& e : items) {
        g.fill_circle(x + 5, y + th / 2, 4.5f, e.color);
        g.stroke_circle(x + 5, y + th / 2, 4.5f, BORDER_HI);
        float w = g.measure(e.text, Font::Tiny).width + 1;
        g.text({x + 16, y, w, th}, e.text, Font::Tiny, MUTED);
        x += 16 + w + 18;
    }

    for (const auto& h : headers_) {
        float w = g.measure(h.title, Font::Caps).width + 2;
        g.text({h.r.x + 4, h.r.y, w, h.r.h - 6}, h.title, Font::Caps, MUTED);
        float line_x = h.r.x + 4 + w + 8;
        g.fill_rect({line_x, std::round(h.r.y + (h.r.h - 6) / 2), std::max(0.0f, h.r.right() - line_x), 1}, BORDER);
    }
    for (auto& row : rows_) row->paint(g);
}

Widget* PadView::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    for (auto& row : rows_)
        if (row->r.contains(x, y)) return row.get();
    if (canvas_->r.contains(x, y)) return canvas_.get();
    return nullptr;
}

void PadView::on_row_click(const std::string& slot, float x, float y) {
    if (contains(spec_.axes, slot)) {
        user_cancel();
        if (cb_.axis_click) cb_.axis_click(slot, x, y);
    } else {
        start_learning(slot);
    }
}

void PadView::on_row_clear(const std::string& slot) {
    if (contains(spec_.axes, slot)) {
        if (cb_.axis_clear) cb_.axis_clear(slot);
    } else if (cb_.clear) {
        cb_.clear(slot);
    }
}

void PadView::user_cancel() {
    bool was = learning_.has_value();
    cancel_learning();
    if (was && cb_.cancel) cb_.cancel();
}

void PadView::on_canvas_click(const std::string& name) {
    if (name.empty()) user_cancel();
    else start_learning(name);
}

void PadView::on_canvas_axis(const std::string& part, float x, float y) {
    user_cancel();
    if (!cb_.axis_click) return;
    if (contains(spec_.axes, part)) {  // spust: od razu wybor zrodla
        cb_.axis_click(part, x, y);
        return;
    }
    auto slots = spec_.stick_slots.find(part);
    if (slots == spec_.stick_slots.end()) return;
    // drazek ma cztery gniazda - najpierw kierunek, potem zrodlo
    PopupMenu menu(std::string(part == "STICK_L" ? "LEWY" : "PRAWY") + " DRĄŻEK - KIERUNEK");
    for (const auto& slot : slots->second) {
        auto label = spec_.row_labels.find(slot);
        auto text = axis_labels_.find(slot);
        menu.add(label == spec_.row_labels.end() ? slot : label->second,
                 [this, slot, x, y] { cb_.axis_click(slot, x, y); }, false,
                 (text == axis_labels_.end() || text->second.empty()) ? EMPTY_SLOT : text->second);
    }
    menu.popup(x, y);
}

void PadView::on_canvas_axis_clear(const std::string& part) {
    if (!cb_.axis_clear) return;
    auto slots = spec_.stick_slots.find(part);
    Names list = slots == spec_.stick_slots.end() ? Names{part} : slots->second;
    for (const auto& slot : list)
        if (contains(spec_.axes, slot)) cb_.axis_clear(slot);
}

void PadView::start_learning(const std::string& name) {
    learning_ = name;
    held_.clear();
    canvas_->set_learning(name);
    pulse_on_ = true;
    pulse_t_ = monotonic() + 0.43;
    canvas_->set_pulse(true);
    refresh_rows();
    relayout();
    if (cb_.assign_request) cb_.assign_request(name);
}

void PadView::cancel_learning() {
    if (!learning_) return;
    learning_.reset();
    held_.clear();
    pulse_on_ = true;
    canvas_->set_pulse(true);
    canvas_->set_learning(std::nullopt);
    refresh_rows();
    relayout();
}

void PadView::tick() {
    double now = monotonic();
    for (auto& row : rows_)
        if (row->flash_until != 0.0 && now >= row->flash_until) {
            row->flash_until = 0.0;
            invalidate();
        }
    if (!learning_ || now < pulse_t_) return;
    pulse_t_ = now + 0.43;
    pulse_on_ = !pulse_on_;
    canvas_->set_pulse(pulse_on_);
    refresh_rows();
}

void PadView::set_wizard(std::optional<int> step, std::optional<int> total) {
    if (step && total) wizard_ = std::make_pair(*step, *total);
    else wizard_.reset();
    relayout();
}

void PadView::set_held_buttons(const std::vector<int>& held) {
    std::vector<int> value = held;
    std::sort(value.begin(), value.end());
    if (value.size() > 6) value.resize(6);
    if (value != held_) {
        held_ = std::move(value);
        if (learning_) relayout();
    }
}

void PadView::on_canvas_hover(const std::string* part) {
    if (!part) {
        apply_hover({}, std::nullopt);
        return;
    }
    auto slots = spec_.slots_of_part.find(*part);
    apply_hover(slots == spec_.slots_of_part.end() ? Names{} : slots->second, *part);
}

void PadView::on_row_hover(const std::string* slot) {
    if (!slot) {
        apply_hover({}, std::nullopt);
        return;
    }
    auto part = spec_.part_of_slot.find(*slot);
    if (part == spec_.part_of_slot.end()) {
        apply_hover({*slot}, std::nullopt);
        return;
    }
    // najechanie na jeden kierunek podswietla caly drazek
    auto slots = spec_.slots_of_part.find(part->second);
    apply_hover(slots == spec_.slots_of_part.end() ? Names{*slot} : slots->second, part->second);
}

void PadView::apply_hover(const Names& slots, const std::optional<std::string>& part) {
    if (slots == hover_slots_) return;
    hover_slots_ = slots;
    canvas_->highlight(part);
    refresh_rows();
}

void PadView::set_assignments(const ButtonMap& assign) {
    bool changed = false;
    for (const auto& name : spec_.controls) {
        auto it = assign.find(name);
        int value = it == assign.end() ? -1 : it->second;
        if (assign_[name] != value) {
            assign_[name] = value;
            changed = true;
            Row* row = row_of_[name];
            row->set_text(value < 0 ? "" : std::format("przycisk {}", value));
            // nauczony przycisk - wiersz potwierdza to na zielono
            if (learning_ && *learning_ == name && value >= 0) row->flash();
        }
    }
    if (changed) {
        std::map<std::string, bool> assigned;
        for (const auto& n : spec_.controls) assigned[n] = assign_[n] >= 0;
        canvas_->set_assigned(assigned);
        refresh_rows();
        if (learning_) relayout();
    }
}

void PadView::set_axis_labels(const LabelMap& labels) {
    bool changed = false;
    for (const auto& name : spec_.axes) {
        auto it = labels.find(name);
        std::string text = it == labels.end() ? "" : it->second;
        if (axis_labels_[name] != text) {
            axis_labels_[name] = text;
            row_of_[name]->set_text(text);
            changed = true;
        }
    }
    if (changed) refresh_rows();
}

void PadView::set_active(const std::map<std::string, bool>& active) {
    bool changed = false;
    for (const auto& name : spec_.controls) {
        auto it = active.find(name);
        bool value = it != active.end() && it->second;
        if (active_[name] != value) {
            active_[name] = value;
            changed = true;
        }
    }
    if (changed) {
        canvas_->set_active(active_);
        refresh_rows();
    }
}

void PadView::set_sticks(double lx, double ly, double rx, double ry) {
    canvas_->set_sticks(lx, ly, rx, ry);
    if (spec_.axes.empty()) return;
    const std::pair<const char*, double> axes[] = {{"LX", lx}, {"LY", ly}, {"RX", rx}, {"RY", ry}};
    for (const auto& [axis, value] : axes) {
        row_of_[std::string(axis) + "+"]->set_value(std::max(0.0, value));
        row_of_[std::string(axis) + "-"]->set_value(std::max(0.0, -value));
    }
}

void PadView::set_triggers(int lt, int rt) {
    canvas_->set_triggers(lt, rt);
    if (spec_.axes.empty()) return;
    row_of_["LT"]->set_value(lt / 255.0);
    row_of_["RT"]->set_value(rt / 255.0);
}

void PadView::refresh_rows() {
    for (auto& row : rows_) {
        std::string state;
        if (learning_ && *learning_ == row->slot) {
            state = "learning";
        } else if (!row->axis) {
            if (active_[row->slot]) state = "active";
            else state = assign_[row->slot] >= 0 ? "set" : "empty";
        } else {
            state = axis_labels_[row->slot].empty() ? "empty" : "set";
        }
        row->set_state(state, contains(hover_slots_, row->slot), pulse_on_);
    }
}

// ------------------------------------------------------------- konfiguracje

const Names G29_WIZARD_ORDER = {"CROSS", "CIRCLE", "SQUARE", "TRIANGLE", "PADDLE_L", "PADDLE_R", "DPAD_UP", "DPAD_DOWN",
                                "DPAD_LEFT", "DPAD_RIGHT", "L2", "R2", "L3", "R3", "SHARE", "OPTIONS", "PS", "PLUS", "MINUS",
                                "DIAL_CW", "DIAL_CCW", "ENTER", "GEAR_1", "GEAR_2", "GEAR_3", "GEAR_4", "GEAR_5", "GEAR_6", "GEAR_R"};

std::unique_ptr<PadView> make_pad_view(PadView::Callbacks callbacks) {
    PadViewSpec s;
    s.canvas = std::make_unique<XboxCanvas>();
    s.controls = PAD_BUTTON_NAMES;
    s.axes = PAD_DIRECTIONS;
    s.groups = {{{"Lewy drążek", true, {"LX-", "LX+", "LY+", "LY-"}},
                 {"Prawy drążek", true, {"RX-", "RX+", "RY+", "RY-"}},
                 {"Spusty analogowe", true, {"LT", "RT"}},
                 {"Bumpery", false, {"LB", "RB"}}},
                {{"Przyciski", false, {"A", "B", "X", "Y"}},
                 {"Krzyżak", false, {"DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"}},
                 {"Klik drążków i środek", false, {"LS", "RS", "BACK", "START", "GUIDE"}}}};
    s.titles = PAD_DIRECTION_LABELS;
    const LabelMap extra = {{"LB", "Lewy bumper (LB)"}, {"RB", "Prawy bumper (RB)"}, {"LS", "Klik lewego drążka (L3)"},
                            {"RS", "Klik prawego drążka (R3)"}, {"A", "Przycisk A"}, {"B", "Przycisk B"}, {"X", "Przycisk X"},
                            {"Y", "Przycisk Y"}, {"BACK", "Back"}, {"START", "Start"}, {"GUIDE", "Guide"},
                            {"DPAD_UP", "Krzyżak w górę"}, {"DPAD_DOWN", "Krzyżak w dół"}, {"DPAD_LEFT", "Krzyżak w lewo"},
                            {"DPAD_RIGHT", "Krzyżak w prawo"}};
    s.titles.insert(extra.begin(), extra.end());
    s.row_labels = pad_row_labels();
    for (const auto& d : PAD_DIRECTIONS)
        if (d != "LT" && d != "RT") s.part_of_slot[d] = d[0] == 'L' ? "STICK_L" : "STICK_R";
    s.part_of_slot["LT"] = "LT";
    s.part_of_slot["RT"] = "RT";
    s.slots_of_part["STICK_L"] = {"LX-", "LX+", "LY+", "LY-"};
    s.slots_of_part["STICK_R"] = {"RX-", "RX+", "RY+", "RY-"};
    s.slots_of_part["LT"] = {"LT"};
    s.slots_of_part["RT"] = {"RT"};
    for (const auto& n : PAD_BUTTON_NAMES) {
        s.part_of_slot[n] = n;
        s.slots_of_part[n] = {n};
    }
    s.stick_slots = {{"STICK_L", {"LX-", "LX+", "LY+", "LY-"}}, {"STICK_R", {"RX-", "RX+", "RY+", "RY-"}}};
    s.row_colors = {{"A", 0x5ac25a}, {"B", 0xe8534a}, {"X", 0x3f9be8}, {"Y", 0xf0c419}};
    s.idle_hint = "Kliknij przycisk na rysunku albo wiersz i wciśnij przycisk kierownicy. Spust lub drążek na rysunku "
                  "otwiera wybór źródła (kierownica, gaz, hamulec…). Prawy przycisk myszy czyści.";
    s.name_minsize = 86;
    return std::make_unique<PadView>(std::move(s), std::move(callbacks));
}

std::unique_ptr<PadView> make_wheel_view(PadView::Callbacks callbacks) {
    PadViewSpec s;
    s.canvas = std::make_unique<WheelCanvas>();
    s.controls = G29_CONTROLS;
    s.groups = {{{"Przyciski", false, {"CROSS", "SQUARE", "CIRCLE", "TRIANGLE"}},
                 {"Łopatki", false, {"PADDLE_L", "PADDLE_R"}},
                 {"Krzyżak", false, {"DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"}},
                 {"L2 / R2 / L3 / R3", false, {"L2", "R2", "L3", "R3"}}},
                {{"Środek", false, {"SHARE", "OPTIONS", "PS"}},
                 {"Pokrętło", false, {"PLUS", "MINUS", "DIAL_CW", "DIAL_CCW", "ENTER"}},
                 {"Skrzynia biegów", false, {"GEAR_1", "GEAR_2", "GEAR_3", "GEAR_4", "GEAR_5", "GEAR_6", "GEAR_R"}}}};
    s.titles = G29_CONTROL_LABELS;
    s.row_labels = G29_CONTROL_LABELS;
    s.row_labels["DPAD_UP"] = "▲ góra";
    s.row_labels["DPAD_DOWN"] = "▼ dół";
    s.row_labels["DPAD_LEFT"] = "◀ lewo";
    s.row_labels["DPAD_RIGHT"] = "▶ prawo";
    s.row_labels["ENTER"] = "Enter";
    for (const auto& n : G29_CONTROLS) {
        s.part_of_slot[n] = n;
        s.slots_of_part[n] = {n};
    }
    s.idle_hint = "Kliknij przycisk na rysunku albo wiersz na liście, a potem wciśnij przycisk na swojej kierownicy. "
                  "Prawy przycisk myszy czyści gniazdo.";
    s.name_minsize = 112;
    return std::make_unique<PadView>(std::move(s), std::move(callbacks));
}

} // namespace wb::ui

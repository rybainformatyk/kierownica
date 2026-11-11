#include "widgets.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "../embedded_ids.h"
#include "../util.h"

namespace wb::ui {

namespace {

constexpr float PI = std::numbers::pi_v<float>;

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

// Cale piksele - linie wlosowe zostaja ostre.
Rect snapped(const Rect& rc) {
    float x = std::round(rc.x), y = std::round(rc.y);
    return {x, y, std::round(rc.right()) - x, std::round(rc.bottom()) - y};
}

float smoothstep(float lo, float hi, float v) {
    float t = clampf((v - lo) / (hi - lo), 0, 1);
    return t * t * (3 - 2 * t);
}

// Najwiekszy krok podzialki, przez ktory dzieli sie kat (wielokrotnosc 10 st.).
// Kreski rzadszego kroku sa dluzsze i zostaja, gdy gestsze sie zlewaja.
int tick_step(int deg) {
    for (int step : {360, 180, 90, 30})
        if (deg % step == 0) return step;
    return 10;
}

} // namespace

bool Spring::step(float dt, float omega, float rest) {
    if (value == target && velocity == 0) return false;
    // rozwiazanie scisle - stabilne takze przy dlugiej klatce
    float d = value - target;
    float c = velocity + omega * d;
    float e = std::exp(-omega * dt);
    value = target + (d + c * dt) * e;
    velocity = (velocity - omega * c * dt) * e;
    if (std::abs(target - value) < rest && std::abs(velocity) < rest * omega) {
        snap(target);
        return false;
    }
    return true;
}

bool Anim::step(float dt, float speed) {
    if (value == target) return false;
    value += (target - value) * (1.0f - std::exp(-speed * dt));
    if (std::abs(target - value) < 0.01f) {
        value = target;
        return false;
    }
    return true;
}

void Widget::set_visible(bool v) {
    if (visible != v) {
        visible = v;
        relayout();
    }
}

std::string upper_pl(const std::string& s) {
    std::wstring w = widen(s);
    if (!w.empty()) CharUpperBuffW(w.data(), static_cast<DWORD>(w.size()));
    return narrow(w);
}

// --------------------------------------------------------------------- Ui

Ui& Ui::get() {
    static Ui instance;
    return instance;
}

void Ui::open_overlay(WidgetPtr overlay) {
    overlay->arrange({0, 0, window.w, window.h});
    overlays_.push_back(std::move(overlay));
    invalidate();
}

void Ui::close_overlay() {
    if (overlays_.empty()) return;
    trash_.push_back(std::move(overlays_.back()));
    overlays_.pop_back();
    invalidate();
}

void Ui::close_all_overlays() {
    while (!overlays_.empty()) close_overlay();
}

Widget* Ui::overlay() { return overlays_.empty() ? nullptr : overlays_.back().get(); }

void Ui::arrange_overlays() {
    for (auto& o : overlays_) o->arrange({0, 0, window.w, window.h});
}

void Ui::paint_overlays(Gfx& g) {
    for (auto& o : overlays_) o->paint(g);
}

bool Ui::in_trash(const Widget* w) const {
    for (const auto& t : trash_)
        if (t.get() == w) return true;
    return false;
}

void Ui::set_focus(Widget* w) {
    if (focus_ == w) return;
    Widget* old = focus_;
    focus_ = w;
    if (old) old->focus_changed(false);
    if (w) w->focus_changed(true);
    invalidate();
}

void Ui::toast(std::string text, RGB color, const char* icon) {
    Toast t{std::move(text), color, icon, time, Anim{}};
    t.show.set(1.0f);
    toasts_.push_back(std::move(t));
    while (toasts_.size() > 4) toasts_.pop_front();
    invalidate();
}

void Ui::tick_toasts(double now) {
    for (auto& t : toasts_)
        if (now - t.born > 3.6 && t.show.target != 0.0f) {
            t.show.set(0.0f);
            invalidate();
        }
    while (!toasts_.empty() && toasts_.front().show.target == 0.0f && toasts_.front().show.value <= 0.01f) {
        toasts_.pop_front();
        invalidate();
    }
}

void Ui::paint_toasts(Gfx& g) {
    float y = window.h - 18;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        animate(it->show, 14);
        float a = it->show.value;
        if (a <= 0.01f) continue;
        float tw = g.measure(it->text, Font::Body).width + 1;
        float w = std::min(window.w - 40, tw + 58);
        float h = 46;
        Rect box{window.w - 18 - w + (1 - a) * 26, y - h, w, h};
        g.begin_opacity(a);
        g.shadow(box, 12, 12, BLACK, 0.55f, 5);
        g.fill_rrect(box, 12, PANEL_HI);
        g.stroke_rrect(box, 12, mix(BORDER_HI, it->color, 0.45));
        g.fill_circle(box.x + 24, box.y + h / 2, 11, it->color, 0.16f);
        g.text({box.x + 13, box.y, 22, h}, it->icon, Font::IconSmall, it->color, Align::Center);
        g.text({box.x + 44, box.y, w - 56, h}, it->text, Font::Body, TEXT);
        g.end_opacity();
        y -= (h + 8) * a;
    }
}

void Ui::set_tooltip(const std::string* text) {
    std::string value = text ? *text : std::string();
    if (value != tooltip_) {
        tooltip_ = std::move(value);
        invalidate();
    }
}

void Ui::paint_tooltip(Gfx& g) {
    if (tooltip_.empty()) return;
    auto s = g.measure(tooltip_, Font::Small, 320);
    float w = s.width + 20, h = s.height + 12;
    float x = clampf(mouse.x + 12, 4, window.w - w - 4);
    float y = mouse.y + 20;
    if (y + h > window.h - 4) y = mouse.y - h - 10;
    Rect box{x, y, w, h};
    g.shadow(box, 8, 8, BLACK, 0.5f, 3);
    g.fill_rrect(box, 8, ELEV);
    g.stroke_rrect(box, 8, BORDER_HI);
    g.text({x + 10, y + 6, s.width + 2, s.height}, tooltip_, Font::Small, TEXT, Align::Left, true);
}

// -------------------------------------------------------------------- Box

Size Box::measure(float avail_w) {
    Size out;
    if (horizontal_) {
        float used = 0;
        float flex = 0;
        for (auto& it : items_) {
            if (!it.w->visible) continue;
            used += it.pack.left + it.pack.right;
            if (it.pack.flex > 0) flex += it.pack.flex;
            else used += it.w->measure(std::max(0.0f, avail_w - used)).w;
        }
        float rest = std::max(0.0f, avail_w - used);
        for (auto& it : items_) {
            if (!it.w->visible) continue;
            float w = it.pack.flex > 0 ? rest * it.pack.flex / flex : it.w->measure(avail_w).w;
            float h = it.w->measure(w).h;
            out.h = std::max(out.h, h + it.pack.top + it.pack.bottom);
        }
        out.w = flex > 0 ? avail_w : used;
    } else {
        for (auto& it : items_) {
            if (!it.w->visible) continue;
            Size s = it.w->measure(std::max(0.0f, avail_w - it.pack.left - it.pack.right));
            out.w = std::max(out.w, s.w + it.pack.left + it.pack.right);
            out.h += s.h + it.pack.top + it.pack.bottom;
        }
    }
    return out;
}

void Box::arrange(const Rect& rc) {
    r = rc;
    if (horizontal_) {
        float used = 0;
        float flex = 0;
        std::vector<float> widths(items_.size(), 0.0f);
        for (size_t i = 0; i < items_.size(); ++i) {
            auto& it = items_[i];
            if (!it.w->visible) continue;
            used += it.pack.left + it.pack.right;
            if (it.pack.flex > 0) {
                flex += it.pack.flex;
            } else {
                widths[i] = it.w->measure(std::max(0.0f, rc.w - used)).w;
                used += widths[i];
            }
        }
        float rest = std::max(0.0f, rc.w - used);
        float x = rc.x;
        for (size_t i = 0; i < items_.size(); ++i) {
            auto& it = items_[i];
            if (!it.w->visible) continue;
            float w = it.pack.flex > 0 ? rest * it.pack.flex / flex : widths[i];
            x += it.pack.left;
            float avail_h = rc.h - it.pack.top - it.pack.bottom;
            float h = avail_h;
            float y = rc.y + it.pack.top;
            if (it.pack.cross != Pack::Stretch) {
                h = std::min(avail_h, it.w->measure(w).h);
                if (it.pack.cross == Pack::Center) y += (avail_h - h) / 2;
                else if (it.pack.cross == Pack::End) y += avail_h - h;
            }
            it.w->arrange(snapped({x, y, w, h}));
            x += w + it.pack.right;
        }
    } else {
        float y = rc.y;
        for (auto& it : items_) {
            if (!it.w->visible) continue;
            float avail_w = rc.w - it.pack.left - it.pack.right;
            Size s = it.w->measure(avail_w);
            y += it.pack.top;
            float w = avail_w;
            float x = rc.x + it.pack.left;
            if (it.pack.cross != Pack::Stretch) {
                w = std::min(avail_w, s.w);
                if (it.pack.cross == Pack::Center) x += (avail_w - w) / 2;
                else if (it.pack.cross == Pack::End) x += avail_w - w;
            }
            it.w->arrange(snapped({x, y, w, s.h}));
            y += s.h + it.pack.bottom;
        }
    }
}

void Box::paint(Gfx& g) {
    if (background_) g.fill_rect(r, *background_);
    for (auto& it : items_)
        if (it.w->visible) it.w->paint(g);
}

Widget* Box::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
        if (Widget* hit = it->w->hit_test(x, y)) return hit;
    return nullptr;
}

// ------------------------------------------------------------------- Grid

std::vector<float> Grid::column_widths(float avail_w) {
    size_t n = weights_.size();
    std::vector<float> widths(n, 0.0f);
    for (size_t c = 0; c < n && c < min_.size(); ++c) widths[c] = min_[c];
    float total_weight = 0;
    for (float w : weights_)
        if (w > 0) total_weight += w;
    for (size_t c = 0; c < n; ++c)
        if (weights_[c] < 0) widths[c] = -weights_[c];  // stala szerokosc

    for (auto& cell : cells_) {
        if (!cell.w->visible || cell.colspan != 1 || weights_[static_cast<size_t>(cell.col)] != 0) continue;
        float w = cell.w->measure(avail_w).w + cell.pack.left + cell.pack.right;
        widths[static_cast<size_t>(cell.col)] = std::max(widths[static_cast<size_t>(cell.col)], w);
    }
    float used = 0;
    for (size_t c = 0; c < n; ++c)
        if (weights_[c] <= 0) used += widths[c];
    float rest = std::max(0.0f, avail_w - used);
    for (size_t c = 0; c < n; ++c)
        if (weights_[c] > 0) widths[c] = std::max(widths[c], rest * weights_[c] / total_weight);
    return widths;
}

std::vector<float> Grid::row_heights(const std::vector<float>& cols, float) {
    int rows = 0;
    for (auto& cell : cells_) rows = std::max(rows, cell.row + 1);
    std::vector<float> heights(static_cast<size_t>(rows), 0.0f);
    for (auto& cell : cells_) {
        if (!cell.w->visible) continue;
        float w = 0;
        for (int c = cell.col; c < cell.col + cell.colspan; ++c) w += cols[static_cast<size_t>(c)];
        w -= cell.pack.left + cell.pack.right;
        float h = cell.w->measure(std::max(0.0f, w)).h + cell.pack.top + cell.pack.bottom;
        heights[static_cast<size_t>(cell.row)] = std::max(heights[static_cast<size_t>(cell.row)], h);
    }
    return heights;
}

Size Grid::measure(float avail_w) {
    auto cols = column_widths(avail_w);
    auto rows = row_heights(cols, avail_w);
    Size out;
    for (float w : cols) out.w += w;
    for (float h : rows) out.h += h;
    return out;
}

void Grid::arrange(const Rect& rc) {
    r = rc;
    auto cols = column_widths(rc.w);
    auto rows = row_heights(cols, rc.w);
    std::vector<float> xs(cols.size() + 1, rc.x), ys(rows.size() + 1, rc.y);
    for (size_t c = 0; c < cols.size(); ++c) xs[c + 1] = xs[c] + cols[c];
    for (size_t row = 0; row < rows.size(); ++row) ys[row + 1] = ys[row] + rows[row];
    for (auto& cell : cells_) {
        if (!cell.w->visible) continue;
        float x = xs[static_cast<size_t>(cell.col)] + cell.pack.left;
        float w = xs[static_cast<size_t>(cell.col + cell.colspan)] - xs[static_cast<size_t>(cell.col)] - cell.pack.left - cell.pack.right;
        float y = ys[static_cast<size_t>(cell.row)] + cell.pack.top;
        float h = rows[static_cast<size_t>(cell.row)] - cell.pack.top - cell.pack.bottom;
        if (cell.pack.cross != Pack::Stretch) {
            Size s = cell.w->measure(w);
            float nw = std::min(w, s.w);
            if (cell.pack.cross == Pack::Center) x += (w - nw) / 2;
            else if (cell.pack.cross == Pack::End) x += w - nw;
            w = nw;
            h = std::min(h, s.h);
        }
        cell.w->arrange(snapped({x, y, w, h}));
    }
}

void Grid::paint(Gfx& g) {
    for (auto& cell : cells_)
        if (cell.w->visible) cell.w->paint(g);
}

Widget* Grid::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    for (auto it = cells_.rbegin(); it != cells_.rend(); ++it)
        if (Widget* hit = it->w->hit_test(x, y)) return hit;
    return nullptr;
}

// ---------------------------------------------------------------- Columns

int Columns::per_row(float w) const {
    int count = static_cast<int>(items_.size());
    for (int n = count; n > 1; --n) {
        bool fits = true;
        for (int first = 0; first < count && fits; first += n) {
            float need = gap_ * static_cast<float>(std::min(n, count - first) - 1);
            for (int i = first; i < std::min(first + n, count); ++i) {
                float weight = static_cast<size_t>(i) < weights_.size() ? weights_[static_cast<size_t>(i)] : 1.0f;
                need += weight < 0 ? -weight : min_col_;
            }
            fits = need <= w;
        }
        if (fits) return n;
    }
    return 1;
}

std::vector<float> Columns::widths(size_t first, size_t count, float w) const {
    std::vector<float> out(count, 0.0f);
    float rest = w - gap_ * static_cast<float>(count - 1);
    float total = 0;
    for (size_t i = 0; i < count; ++i) {
        float weight = first + i < weights_.size() ? weights_[first + i] : 1.0f;
        if (weight < 0) {
            out[i] = -weight;
            rest -= -weight;
        } else {
            total += weight;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        float weight = first + i < weights_.size() ? weights_[first + i] : 1.0f;
        if (weight >= 0) out[i] = count == 1 ? w : rest * weight / total;
    }
    if (count == 1) out[0] = w;
    return out;
}

Size Columns::measure(float avail_w) {
    int n = per_row(avail_w);
    Size out{avail_w, 0};
    for (size_t first = 0; first < items_.size(); first += static_cast<size_t>(n)) {
        size_t count = std::min(static_cast<size_t>(n), items_.size() - first);
        auto w = widths(first, count, avail_w);
        float h = 0;
        for (size_t i = 0; i < count; ++i)
            if (items_[first + i]->visible) h = std::max(h, items_[first + i]->measure(w[i]).h);
        out.h += h + (first ? gap_ : 0);
    }
    return out;
}

void Columns::arrange(const Rect& rc) {
    r = rc;
    int n = per_row(rc.w);
    float y = rc.y;
    for (size_t first = 0; first < items_.size(); first += static_cast<size_t>(n)) {
        size_t count = std::min(static_cast<size_t>(n), items_.size() - first);
        auto w = widths(first, count, rc.w);
        float h = 0;
        for (size_t i = 0; i < count; ++i)
            if (items_[first + i]->visible) h = std::max(h, items_[first + i]->measure(w[i]).h);
        float x = rc.x;
        for (size_t i = 0; i < count; ++i) {
            if (items_[first + i]->visible) items_[first + i]->arrange(snapped({x, y, w[i], h}));
            x += w[i] + gap_;
        }
        y += h + gap_;
    }
}

void Columns::paint(Gfx& g) {
    for (auto& w : items_)
        if (w->visible) w->paint(g);
}

Widget* Columns::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
        if (Widget* hit = (*it)->hit_test(x, y)) return hit;
    return nullptr;
}

// ------------------------------------------------------------------- Card

namespace {
constexpr float CARD_PX = 20, CARD_PY = 18, CARD_RADIUS = 14;
}

Card::Card(std::string title, std::string subtitle, float wrap)
    : title_(std::move(title)), body_(std::make_unique<Box>(false)), right_(std::make_unique<Box>(true)) {
    body = body_.get();
    right_slot = right_.get();
    if (!subtitle.empty())
        body_->add(std::make_unique<Label>(std::move(subtitle), Font::Small, MUTED, wrap), Pack{.bottom = 10});
}

float Card::header_h(float inner_w) {
    if (title_.empty() && right_->empty()) return 0;
    float title = title_.empty() ? 0 : Gfx::get().measure(title_, Font::Head).height;
    return std::max(title, right_->measure(inner_w).h);
}

Size Card::measure(float avail_w) {
    float inner = std::max(0.0f, avail_w - 2 * CARD_PX);
    float head = header_h(inner);
    return {avail_w, CARD_PY * 2 + (head > 0 ? head + 14 : 0) + body_->measure(inner).h};
}

void Card::arrange(const Rect& rc) {
    r = rc;
    float inner = std::max(0.0f, rc.w - 2 * CARD_PX);
    float head = header_h(inner);
    float rw = right_->measure(inner).w;
    right_->arrange(snapped({rc.right() - CARD_PX - rw, rc.y + CARD_PY, rw, head}));
    float top = rc.y + CARD_PY + (head > 0 ? head + 14 : 0);
    body_->arrange(snapped({rc.x + CARD_PX, top, inner, std::max(0.0f, rc.bottom() - CARD_PY - top)}));
}

void Card::paint(Gfx& g) {
    g.fill_rrect_v(r, CARD_RADIUS, 0x161b25, 0x11151c);
    g.stroke_rrect(r, CARD_RADIUS, BORDER);
    // cienkie swiatlo na gornej krawedzi - karta lekko uniesiona nad tlem
    g.stroke_rrect_v(r, CARD_RADIUS, WHITE, WHITE, 0.06f, 0.0f);
    if (!title_.empty()) {
        float inner = std::max(0.0f, r.w - 2 * CARD_PX);
        g.text({r.x + CARD_PX, r.y + CARD_PY, inner, header_h(inner)}, title_, Font::Head, TEXT);
    }
    right_->paint(g);
    body_->paint(g);
}

Widget* Card::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    if (Widget* hit = right_->hit_test(x, y)) return hit;
    return body_->hit_test(x, y);
}

// ------------------------------------------------------------- SettingRow

namespace {
constexpr float ROW_PY = 12;
}

bool SettingRow::stacked(float w) const {
    // kontrolka bez stalej szerokosci (np. kilka przyciskow) tez musi zostawic miejsce na opis
    return control_w_ > 0 ? w < control_w_ + 240 : w < control_->measure(w).w + 160;
}

float SettingRow::control_width(float w) {
    if (control_w_ > 0) return stacked(w) ? w : control_w_;
    return std::min(w, control_->measure(w).w);
}

float SettingRow::text_width(float w) {
    if (stacked(w)) return w;
    return std::max(120.0f, w - control_width(w) - 24);
}

Size SettingRow::measure(float avail_w) {
    Gfx& g = Gfx::get();
    float tw = text_width(avail_w);
    float text_h = g.measure(title_, Font::BodyStrong, tw).height;
    if (!desc_.empty()) text_h += 2 + g.measure(desc_, Font::Small, tw).height;
    float control_h = control_->measure(control_width(avail_w)).h;
    if (stacked(avail_w)) return {avail_w, ROW_PY * 2 + text_h + 8 + control_h};
    return {avail_w, ROW_PY * 2 + std::max(text_h, control_h)};
}

void SettingRow::arrange(const Rect& rc) {
    r = rc;
    Gfx& g = Gfx::get();
    float cw = control_width(rc.w);
    float ch = control_->measure(cw).h;
    if (stacked(rc.w)) {
        control_->arrange(snapped({rc.x, rc.bottom() - ROW_PY - ch, cw, ch}));
    } else {
        float tw = text_width(rc.w);
        float text_h = g.measure(title_, Font::BodyStrong, tw).height;
        if (!desc_.empty()) text_h += 2 + g.measure(desc_, Font::Small, tw).height;
        float rows = std::max(text_h, ch);
        control_->arrange(snapped({rc.right() - cw, rc.y + ROW_PY + (rows - ch) / 2, cw, ch}));
    }
}

void SettingRow::paint(Gfx& g) {
    if (divider_) g.fill_rect({r.x, r.y, r.w, 1}, BORDER, 0.7f);
    float tw = text_width(r.w);
    float title_h = g.measure(title_, Font::BodyStrong, tw).height;
    float y = r.y + ROW_PY;
    if (!stacked(r.w)) {
        float text_h = title_h;
        if (!desc_.empty()) text_h += 2 + g.measure(desc_, Font::Small, tw).height;
        float avail = r.h - 2 * ROW_PY;
        y += std::max(0.0f, (avail - text_h) / 2);
    }
    g.text({r.x, y, tw, title_h}, title_, Font::BodyStrong, TEXT, Align::Left, true);
    if (!desc_.empty()) {
        float desc_h = g.measure(desc_, Font::Small, tw).height;
        g.text({r.x, y + title_h + 2, tw, desc_h}, desc_, Font::Small, MUTED, Align::Left, true);
    }
    control_->paint(g);
}

Widget* SettingRow::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    return control_->hit_test(x, y);
}

// ----------------------------------------------------------------- Scroll

void Scroll::arrange(const Rect& rc) {
    r = rc;
    float w = std::max(1.0f, rc.w - 2 * pad_x_);
    content_h_ = content_->measure(w).h + 2 * pad_y_;
    offset_ = clampf(offset_, 0, std::max(0.0f, content_h_ - rc.h));
    content_->arrange(snapped({rc.x + pad_x_, rc.y + pad_y_ - offset_, w, content_h_ - 2 * pad_y_}));
}

void Scroll::set_offset(float offset) {
    offset = clampf(offset, 0, std::max(0.0f, content_h_ - r.h));
    if (offset != offset_) {
        offset_ = offset;
        relayout();
    }
}

bool Scroll::mouse_wheel(int delta) {
    if (content_h_ <= r.h) return false;
    // 120 = jeden zabek kolka = 3 kroki po 24 px; gladzik wysyla ulamki
    wheel_acc_ -= static_cast<float>(delta) / 40.0f;
    int steps = static_cast<int>(wheel_acc_);
    if (steps) {
        wheel_acc_ -= static_cast<float>(steps);
        set_offset(offset_ + static_cast<float>(steps) * 24.0f);
    }
    return true;
}

Rect Scroll::thumb() const {
    float h = std::max(32.0f, r.h * r.h / content_h_);
    float span = r.h - h - 8;
    float max_off = content_h_ - r.h;
    float y = r.y + 4 + (max_off > 0 ? span * offset_ / max_off : 0);
    return {r.right() - BAR_W - 3, y, BAR_W, h};
}

void Scroll::paint(Gfx& g) {
    g.push_clip(r);
    content_->paint(g);
    g.pop_clip();
    if (content_h_ > r.h) g.fill_rrect(thumb(), 3, dragging_ ? ACCENT_DIM : BORDER_HI, dragging_ ? 1.0f : 0.8f);
}

Widget* Scroll::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    if (content_h_ > r.h && x >= r.right() - BAR_W - 6) return this;
    if (Widget* hit = content_->hit_test(x, y)) return hit;
    return this;
}

void Scroll::mouse_down(float x, float y) {
    if (content_h_ <= r.h || x < r.right() - BAR_W - 6) return;
    Rect t = thumb();
    if (!t.contains(x, y)) {
        // klik w rowek przenosi uchwyt pod kursor
        float span = r.h - t.h - 8;
        if (span > 0) set_offset((y - r.y - 4 - t.h / 2) / span * (content_h_ - r.h));
    }
    dragging_ = true;
    drag_y_ = y;
    drag_offset_ = offset_;
    invalidate();
}

void Scroll::mouse_move(float, float y) {
    if (!dragging_) return;
    float span = r.h - thumb().h - 8;
    if (span > 0) set_offset(drag_offset_ + (y - drag_y_) / span * (content_h_ - r.h));
}

void Scroll::mouse_up(float, float) {
    dragging_ = false;
    invalidate();
}

// ------------------------------------------------------------------ Pages

int Pages::add(WidgetPtr page) {
    pages_.push_back(std::move(page));
    return static_cast<int>(pages_.size()) - 1;
}

void Pages::select(int index) {
    if (index < 0 || index >= static_cast<int>(pages_.size()) || index == current_) return;
    current_ = index;
    Ui::get().set_focus(nullptr);
    show_.snap(0.0f);
    show_.set(1.0f);
    relayout();
    if (on_changed) on_changed(index);
}

void Pages::arrange(const Rect& rc) {
    r = rc;
    pages_[static_cast<size_t>(current_)]->arrange(rc);
}

void Pages::paint(Gfx& g) {
    animate(show_, 16);
    auto& page = *pages_[static_cast<size_t>(current_)];
    float t = show_.value;
    if (t >= 0.995f) {
        page.paint(g);
        return;
    }
    g.begin_opacity(t);
    g.push_translate(0, (1.0f - t) * 12.0f);
    page.paint(g);
    g.pop_translate();
    g.end_opacity();
}

Widget* Pages::hit_test(float x, float y) { return pages_[static_cast<size_t>(current_)]->hit_test(x, y); }

// ------------------------------------------------------------------ Label

void Label::set(const std::string& text) {
    if (text_ == text) return;
    bool layout = fixed_w_ <= 0;
    text_ = text;
    if (layout) relayout();
    else invalidate();
}

void Label::set_color(RGB color) {
    if (color_ != color) {
        color_ = color;
        invalidate();
    }
}

Size Label::measure(float avail_w) {
    Gfx& g = Gfx::get();
    if (fixed_w_ > 0) return {fixed_w_, g.measure("Ag", font_).height};
    if (cap_ > 0) {
        float w = std::max(120.0f, std::min(cap_, avail_w));
        auto s = g.measure(text_, font_, w);
        return {std::min(s.width + 1, w), s.height};
    }
    auto s = g.measure(text_, font_);
    return {s.width + 1, s.height};
}

void Label::paint(Gfx& g) {
    bool wrap = cap_ > 0;
    g.text(wrap ? Rect{r.x, r.y, std::max(120.0f, std::min(cap_, r.w + 1)), r.h} : r, text_, font_, color_, align_, wrap);
}

// ----------------------------------------------------------------- Button

Button::Button(std::string text, Style style, std::function<void()> command, float width)
    : text_(std::move(text)), style_(style), command_(std::move(command)), width_(width) {}

void Button::set(const std::string& text) {
    if (text_ != text) {
        text_ = text;
        relayout();
    }
}

void Button::set_style(Style s) {
    if (style_ != s) {
        style_ = s;
        relayout();
    }
}

void Button::set_enabled(bool e) {
    if (enabled_ != e) {
        enabled_ = e;
        invalidate();
    }
}

Size Button::measure(float) {
    bool compact = style_ == Small || style_ == Ghost;
    auto s = Gfx::get().measure(text_, compact ? Font::SmallStrong : Font::BodyStrong);
    float px = compact ? 12.0f : 16.0f;
    float icon_w = icon_ ? 21.0f : 0.0f;
    return {width_ > 0 ? width_ : s.width + 2 * px + icon_w + 2, compact ? 30.0f : 36.0f};
}

void Button::paint(Gfx& g) {
    hover_anim_.set(hover_ && enabled_ ? 1.0f : 0.0f);
    animate(hover_anim_, 22);
    float h = hover_anim_.value;
    bool down = pressed_ && hover_ && enabled_;
    bool compact = style_ == Small || style_ == Ghost;
    const float radius = 9;
    RGB fg = TEXT;
    switch (style_) {
    case Accent: {
        RGB fill = enabled_ ? (down ? 0x2f9fd8 : mix(ACCENT, ACCENT_HI, h)) : ACCENT_DIM;
        fg = enabled_ ? ACCENT_INK : 0x4d6678;
        if (enabled_ && h > 0.01f) g.shadow(r, radius, 9, ACCENT, 0.30f * h, 2);
        g.fill_rrect_v(r, radius, lighten(fill, 0.07), fill);
        break;
    }
    case Normal:
    case Small: {
        RGB fill = mix(PANEL_HI, ELEV, h), edge = mix(BORDER, BORDER_HI, h);
        if (down) { fill = ACCENT_DEEP; edge = ACCENT_DIM; }
        if (!enabled_) { fill = PANEL; edge = BORDER; fg = MUTED; }
        g.fill_rrect(r, radius, fill);
        g.stroke_rrect(r, radius, edge);
        break;
    }
    case Ghost:
        if (h > 0.01f) g.fill_rrect(r, radius, ELEV, h);
        fg = mix(TEXT_DIM, TEXT, h);
        break;
    case Danger: {
        RGB fill = mix(0x3a2028, 0x4a2a31, h);
        g.fill_rrect(r, radius, fill);
        g.stroke_rrect(r, radius, 0x5a3039);
        fg = BAD;
        break;
    }
    }
    Font font = compact ? Font::SmallStrong : Font::BodyStrong;
    if (icon_) {
        float tw = g.measure(text_, font).width + 1;
        float x = r.x + (r.w - tw - 21) / 2;
        g.text({x, r.y, 16, r.h}, icon_, Font::IconSmall, fg, Align::Center);
        g.text({x + 21, r.y, tw + 1, r.h}, text_, font, fg);
    } else {
        g.text(r, text_, font, fg, Align::Center);
    }
}

void Button::mouse_up(float x, float y) {
    bool click = pressed_ && r.contains(x, y) && enabled_;
    pressed_ = false;
    invalidate();
    if (click && command_) command_();
}

void IconButton::paint(Gfx& g) {
    hover_anim_.set(hover_ ? 1.0f : 0.0f);
    animate(hover_anim_, 22);
    float h = hover_anim_.value;
    if (h > 0.01f) g.fill_rrect(r, 9, ELEV, h);
    g.text(r, icon_, Font::IconSmall, mix(TEXT_DIM, TEXT, h), Align::Center);
}

void IconButton::mouse_up(float x, float y) {
    if (r.contains(x, y) && command_) command_();
}

// ----------------------------------------------------------------- Toggle

Toggle::Toggle(bool value, std::function<void(bool)> command)
    : value_(value), command_(std::move(command)), pos_(value ? 1.0f : 0.0f) {}

void Toggle::set(bool v) {
    if (value_ != v) {
        value_ = v;
        invalidate();
    }
}

void Toggle::paint(Gfx& g) {
    pos_.set(value_ ? 1.0f : 0.0f);
    hover_anim_.set(hover_ ? 1.0f : 0.0f);
    animate(pos_, 20);
    animate(hover_anim_, 22);
    float p = pos_.value, h = hover_anim_.value;
    Rect t{r.x, r.y + (r.h - 24) / 2, 42, 24};
    RGB off = mix(TRACK, ELEV, h);
    g.fill_rrect(t, 12, mix(off, mix(ACCENT, ACCENT_HI, h), p));
    if (p < 0.99f) g.stroke_rrect(t, 12, BORDER_HI, 1.0f, 1.0f - p);
    float x = t.x + 12 + p * 18, cy = t.y + 12;
    g.fill_circle(x, cy + 0.5f, 9.5f, BLACK, 0.25f);
    g.fill_circle(x, cy, 9, mix(TEXT_DIM, WHITE, std::max(p, h)));
}

void Toggle::mouse_up(float x, float y) {
    if (!r.contains(x, y)) return;
    value_ = !value_;
    invalidate();
    if (command_) command_(value_);
}

// -------------------------------------------------------------- Segmented

Segmented::Segmented(std::vector<std::string> items, int index, std::function<void(int)> on_change, float height)
    : items_(std::move(items)), index_(index), on_change_(std::move(on_change)), height_(height) {}

void Segmented::set_index(int index) {
    if (index_ != index) {
        index_ = index;
        invalidate();
    }
}

float Segmented::item_w() const { return (r.w - 4) / static_cast<float>(items_.size()); }

Size Segmented::measure(float avail_w) {
    Gfx& g = Gfx::get();
    float widest = 0;
    for (const auto& s : items_) widest = std::max(widest, g.measure(s, Font::SmallStrong).width);
    float w = (widest + 20) * static_cast<float>(items_.size()) + 4;
    return {avail_w > 0 ? std::min(w, avail_w) : w, height_};
}

int Segmented::item_at(float x) const {
    int i = static_cast<int>((x - r.x - 2) / item_w());
    return (i >= 0 && i < static_cast<int>(items_.size())) ? i : -1;
}

void Segmented::paint(Gfx& g) {
    g.fill_rrect(r, 10, PANEL_HI);
    g.stroke_rrect(r, 10, BORDER);
    float w = item_w();
    if (!placed_ && index_ >= 0) {
        pos_.snap(static_cast<float>(index_));
        placed_ = true;
    }
    if (index_ >= 0) {
        pos_.set(static_cast<float>(index_));
        if (pos_.step(Ui::get().dt, 20.0f, 0.002f)) invalidate();
        Rect sel{r.x + 2 + pos_.value * w, r.y + 2, w, r.h - 4};
        g.shadow(sel, 8, 8, ACCENT, 0.28f, 1);
        g.fill_rrect_v(sel, 8, ACCENT_HI, ACCENT);
    }
    for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
        Rect cell{r.x + 2 + static_cast<float>(i) * w, r.y + 2, w, r.h - 4};
        bool sel = i == index_;
        if (!sel && i == hover_) g.fill_rrect(cell.inset(1), 7, ELEV);
        // napis ciemnieje, gdy wjezdza pod niego znacznik wyboru
        float under = index_ >= 0 ? clampf(1.0f - std::abs(static_cast<float>(i) - pos_.value), 0, 1) : 0.0f;
        g.text(cell, items_[static_cast<size_t>(i)], Font::SmallStrong, mix(i == hover_ ? TEXT : TEXT_DIM, ACCENT_INK, under), Align::Center);
    }
}

void Segmented::mouse_move(float x, float) {
    int i = item_at(x);
    if (i != hover_) {
        hover_ = i;
        invalidate();
    }
}

void Segmented::mouse_leave() {
    hover_ = -1;
    invalidate();
}

void Segmented::mouse_down(float x, float) {
    int i = item_at(x);
    if (i >= 0 && i != index_) {
        index_ = i;
        invalidate();
        if (on_change_) on_change_(i);
    }
}

// ------------------------------------------------------------------ Combo

namespace {

// Rozwinieta lista - nakladka na okno, klik obok ja zamyka.
class ComboList final : public Widget {
public:
    static constexpr float ROW = 30;
    static constexpr int MAX_ROWS = 12;

    ComboList(const Rect& anchor, std::vector<std::string> items, int index, std::function<void(int)> pick)
        : items_(std::move(items)), selected_(index), pick_(std::move(pick)) {
        Gfx& g = Gfx::get();
        float width = anchor.w;
        for (const auto& s : items_) width = std::max(width, g.measure(s, Font::Body).width + 44);
        int shown = std::min(static_cast<int>(items_.size()), MAX_ROWS);
        float h = static_cast<float>(shown) * ROW + 10;
        Size win = Ui::get().window;
        width = std::min(width, win.w - 16);
        float x = clampf(anchor.x, 8, win.w - width - 8);
        float y = anchor.bottom() + 4;
        if (y + h > win.h - 6) y = std::max(6.0f, anchor.y - h - 4);
        box_ = {x, y, width, h};
        // od razu widac biezacy wybor
        first_ = std::clamp(index - shown / 2, 0, std::max(0, static_cast<int>(items_.size()) - shown));
        hover_ = index;
        show_.set(1.0f);
    }

    Size measure(float) override { return Ui::get().window; }
    Widget* hit_test(float, float) override { return this; }

    void paint(Gfx& g) override {
        animate(show_, 26);
        float a = show_.value;
        g.begin_opacity(a);
        g.shadow(box_, 10, 14, BLACK, 0.6f, 6);
        g.fill_rrect(box_, 10, 0x1f2631);
        g.stroke_rrect(box_, 10, BORDER_HI);
        Rect in = box_.inset(5);
        g.push_clip(in);
        int shown = static_cast<int>(in.h / ROW);
        for (int i = 0; i < shown && first_ + i < static_cast<int>(items_.size()); ++i) {
            int idx = first_ + i;
            Rect row{in.x, in.y + static_cast<float>(i) * ROW, in.w, ROW};
            bool hot = idx == hover_;
            if (hot) g.fill_rrect(row, 7, ELEV);
            if (idx == selected_) g.text({row.x + 6, row.y, 18, ROW}, ic::check, Font::IconSmall, ACCENT, Align::Center);
            g.text_fit({row.x + 28, row.y, row.w - 36, ROW}, items_[static_cast<size_t>(idx)], idx == selected_ ? Font::BodyStrong : Font::Body, hot || idx == selected_ ? TEXT : TEXT_DIM);
        }
        g.pop_clip();
        if (static_cast<int>(items_.size()) > shown) {
            float frac = static_cast<float>(shown) / static_cast<float>(items_.size());
            float top = static_cast<float>(first_) / static_cast<float>(items_.size());
            g.fill_rrect({box_.right() - 7, in.y + top * in.h, 3, std::max(18.0f, frac * in.h)}, 1.5f, BORDER_HI);
        }
        g.end_opacity();
    }

    void mouse_move(float x, float y) override {
        int i = row_at(x, y);
        if (i != hover_ && i >= 0) {
            hover_ = i;
            invalidate();
        }
    }

    void mouse_down(float x, float y) override {
        if (!box_.contains(x, y)) {
            Ui::get().close_overlay();
            return;
        }
        pressed_inside_ = true;
    }

    void mouse_up(float x, float y) override {
        if (!pressed_inside_) return;
        int i = row_at(x, y);
        auto pick = pick_;
        Ui::get().close_overlay();
        if (i >= 0 && pick) pick(i);
    }

    bool mouse_wheel(int delta) override {
        int shown = std::min(static_cast<int>(items_.size()), MAX_ROWS);
        int top = std::max(0, static_cast<int>(items_.size()) - shown);
        first_ = std::clamp(first_ - (delta > 0 ? 3 : -3), 0, top);
        invalidate();
        return true;
    }

    bool key_down(unsigned vk) override {
        int count = static_cast<int>(items_.size());
        int shown = std::min(count, MAX_ROWS);
        if (vk == VK_ESCAPE) {
            Ui::get().close_overlay();
            return true;
        }
        if (vk == VK_UP || vk == VK_DOWN) {
            hover_ = std::clamp(hover_ + (vk == VK_UP ? -1 : 1), 0, count - 1);
            if (hover_ < first_) first_ = hover_;
            if (hover_ >= first_ + shown) first_ = hover_ - shown + 1;
            invalidate();
            return true;
        }
        if (vk == VK_RETURN && hover_ >= 0) {
            auto pick = pick_;
            int i = hover_;
            Ui::get().close_overlay();
            if (pick) pick(i);
            return true;
        }
        return false;
    }

    bool hand_cursor() const override { return true; }

private:
    int row_at(float x, float y) const {
        Rect in = box_.inset(5);
        if (!in.contains(x, y)) return -1;
        int i = first_ + static_cast<int>((y - in.y) / ROW);
        return i < static_cast<int>(items_.size()) ? i : -1;
    }

    Rect box_;
    std::vector<std::string> items_;
    int selected_;
    int hover_ = -1;
    int first_ = 0;
    bool pressed_inside_ = false;
    std::function<void(int)> pick_;
    Anim show_;
};

} // namespace

Combo::Combo(std::vector<std::string> items, int index, std::function<void(int)> on_change, int width_chars)
    : items_(std::move(items)), index_(index), on_change_(std::move(on_change)), width_chars_(width_chars) {}

void Combo::set_items(std::vector<std::string> items, int index) {
    items_ = std::move(items);
    index_ = index;
    invalidate();
}

void Combo::set_index(int index) {
    if (index_ != index) {
        index_ = index;
        invalidate();
    }
}

const std::string& Combo::text() const {
    static const std::string empty;
    return (index_ >= 0 && index_ < static_cast<int>(items_.size())) ? items_[static_cast<size_t>(index_)] : empty;
}

Size Combo::measure(float avail_w) {
    float w = static_cast<float>(width_chars_) * 7.4f + 18 + 26;
    return {avail_w > 0 ? std::min(w, std::max(avail_w, 60.0f)) : w, 36};
}

void Combo::paint(Gfx& g) {
    hover_anim_.set(hover_ ? 1.0f : 0.0f);
    animate(hover_anim_, 22);
    float h = hover_anim_.value;
    g.fill_rrect(r, 9, mix(PANEL_HI, ELEV, h));
    g.stroke_rrect(r, 9, mix(BORDER, BORDER_HI, h));
    g.text_fit({r.x + 12, r.y, r.w - 12 - 28, r.h}, text(), Font::Body, TEXT);
    g.text({r.right() - 28, r.y, 22, r.h}, ic::chevron_down, Font::IconSmall, mix(MUTED, TEXT, h), Align::Center);
}

void Combo::mouse_down(float, float) {
    auto pick = [this](int i) {
        index_ = i;
        invalidate();
        if (on_change_) on_change_(i);
    };
    Ui::get().open_overlay(std::make_unique<ComboList>(r, items_, index_, pick));
}

// ------------------------------------------------------------------ Entry

void Entry::set(const std::string& text) {
    if (text_ != text) {
        text_ = text;
        caret_ = text_.size();
        invalidate();
    }
}

Size Entry::measure(float) { return {static_cast<float>(width_chars_) * 7.4f + 22, 36}; }

void Entry::paint(Gfx& g) {
    bool focus = Ui::get().focus() == this;
    g.fill_rrect(r, 9, PANEL_HI);
    g.stroke_rrect(r, 9, focus ? ACCENT : BORDER);
    g.text({r.x + 11, r.y, r.w - 22, r.h}, text_, Font::Num, TEXT);
    if (focus) {
        caret_ = std::min(caret_, text_.size());
        float x = r.x + 11 + (caret_ ? g.measure(text_.substr(0, caret_), Font::Num).width : 0);
        g.line(x, r.y + 8, x, r.bottom() - 8, ACCENT, 1.4f);
    }
}

void Entry::mouse_down(float, float) {
    Ui::get().set_focus(this);
    caret_ = text_.size();
}

bool Entry::key_down(unsigned vk) {
    switch (vk) {
    case VK_LEFT: if (caret_ > 0) --caret_; break;
    case VK_RIGHT: if (caret_ < text_.size()) ++caret_; break;
    case VK_HOME: caret_ = 0; break;
    case VK_END: caret_ = text_.size(); break;
    case VK_BACK:
        if (caret_ > 0) text_.erase(--caret_, 1);
        break;
    case VK_DELETE:
        if (caret_ < text_.size()) text_.erase(caret_, 1);
        break;
    case VK_RETURN:
    case VK_TAB:
        Ui::get().set_focus(nullptr);  // zatwierdza w focus_changed
        break;
    case VK_ESCAPE:
        text_ = before_edit_;
        caret_ = text_.size();
        Ui::get().set_focus(nullptr);
        break;
    default:
        return false;
    }
    invalidate();
    return true;
}

void Entry::focus_changed(bool focused) {
    if (focused) {
        before_edit_ = text_;
    } else if (on_commit_ && text_ != before_edit_) {
        before_edit_ = text_;
        on_commit_(text_);
    }
    invalidate();
}

void Entry::char_input(wchar_t c) {
    if (c < 32 || c > 126) return;
    text_.insert(caret_++, 1, static_cast<char>(c));
    invalidate();
}

// ---------------------------------------------------------------- Stepper

Stepper::Stepper(int min_value, int max_value, int value, std::function<void(int)> command, std::string zero_text)
    : min_(min_value), max_(max_value), value_(value), command_(std::move(command)), zero_text_(std::move(zero_text)) {}

void Stepper::set(int v) {
    if (value_ != v) {
        value_ = v;
        invalidate();
    }
}

int Stepper::zone(float x) const {
    if (x < r.x + 36) return -1;
    if (x >= r.right() - 36) return 1;
    return 0;
}

void Stepper::paint(Gfx& g) {
    g.fill_rrect(r, 9, PANEL_HI);
    g.stroke_rrect(r, 9, BORDER);
    Rect minus{r.x + 2, r.y + 2, 32, r.h - 4}, plus{r.right() - 34, r.y + 2, 32, r.h - 4};
    if (hover_ < 0) g.fill_rrect(minus, 7, ELEV);
    if (hover_ > 0) g.fill_rrect(plus, 7, ELEV);
    g.text(minus, ic::remove, Font::IconSmall, value_ > min_ ? TEXT_DIM : BORDER_HI, Align::Center);
    g.text(plus, ic::add, Font::IconSmall, value_ < max_ ? TEXT_DIM : BORDER_HI, Align::Center);
    bool zero = value_ == 0 && !zero_text_.empty();
    g.text({r.x + 34, r.y, r.w - 68, r.h}, zero ? zero_text_ : std::to_string(value_), zero ? Font::Small : Font::Num, zero ? MUTED : TEXT, Align::Center);
}

void Stepper::mouse_move(float x, float) {
    int z = zone(x);
    if (z != hover_) {
        hover_ = z;
        invalidate();
    }
}

void Stepper::mouse_leave() {
    hover_ = 0;
    invalidate();
}

void Stepper::mouse_down(float x, float) {
    int z = zone(x);
    if (!z) return;
    int v = std::clamp(value_ + z, min_, max_);
    if (v != value_) {
        value_ = v;
        invalidate();
        if (command_) command_(v);
    }
}

// ------------------------------------------------------------------- Chip

Chip::Chip(std::string text, RGB color, std::function<void()> command, bool chevron)
    : text_(std::move(text)), color_(color), command_(std::move(command)), chevron_(chevron) {}

void Chip::set(const std::string& text, RGB color) {
    if (text_ != text) {
        text_ = text;
        relayout();
    }
    if (color_ != color) {
        color_ = color;
        invalidate();
    }
}

void Chip::set_spinner(bool on) {
    if (spinner_ != on) {
        spinner_ = on;
        invalidate();
    }
}

Size Chip::measure(float) {
    auto s = Gfx::get().measure(text_, Font::SmallStrong);
    return {s.width + 12 + 18 + (chevron_ ? 26.0f : 12.0f), 36};
}

void Chip::paint(Gfx& g) {
    hover_anim_.set(hover_ ? 1.0f : 0.0f);
    animate(hover_anim_, 22);
    float h = hover_anim_.value;
    g.fill_rrect(r, 18, mix(PANEL_HI, ELEV, h));
    g.stroke_rrect(r, 18, mix(mix(BORDER, color_, 0.25), BORDER_HI, h));
    float cy = r.y + r.h / 2;
    if (spinner_) {
        g.spinner(r.x + 16, cy, 5, color_, Ui::get().time);
        invalidate();
    } else {
        g.fill_circle(r.x + 16, cy, 7, color_, 0.2f);
        g.fill_circle(r.x + 16, cy, 3.5f, color_);
    }
    float right = chevron_ ? 28.0f : 12.0f;
    g.text_fit({r.x + 28, r.y, r.w - 28 - right, r.h}, text_, Font::SmallStrong, TEXT);
    if (chevron_) g.text({r.right() - 24, r.y, 18, r.h}, ic::chevron_down, Font::IconSmall, MUTED, Align::Center);
}

void Chip::mouse_up(float x, float y) {
    if (r.contains(x, y) && command_) command_();
}

// ----------------------------------------------------------------- Slider

namespace {
constexpr float BUBBLE_W = 62, RESET_W = 24;
}

Slider::Slider(double from, double to, double value, Unit unit, double resolution, std::function<void(double)> command, std::optional<double> default_value)
    : from_(from), to_(to), value_(value), resolution_(resolution), unit_(unit), command_(std::move(command)), default_(default_value) {}

double Slider::snap(double v) const {
    double lo = std::min(from_, to_), hi = std::max(from_, to_);
    v = std::max(lo, std::min(hi, v));
    if (resolution_ != 0.0) {
        v = std::rint(v / resolution_) * resolution_;
        v = std::max(lo, std::min(hi, v));
    }
    return v;
}

double Slider::get() const { return snap(value_); }

void Slider::set(double value) {
    if (value_ != value) {
        value_ = value;
        invalidate();
    }
}

void Slider::apply(double value) {
    if (std::abs(value - value_) < 1e-9) return;
    value_ = value;
    invalidate();
    if (command_) command_(value);
}

Rect Slider::track_rect() const { return {r.x, r.y, std::max(40.0f, r.w - BUBBLE_W - RESET_W - 10), r.h}; }
Rect Slider::bubble_rect() const { return {track_rect().right() + 10, r.y + (r.h - 28) / 2, BUBBLE_W, 28}; }
Rect Slider::reset_rect() const { return {bubble_rect().right(), r.y + (r.h - 28) / 2, RESET_W, 28}; }
bool Slider::changed() const { return default_ && std::abs(snap(*default_) - snap(value_)) > 1e-9; }

double Slider::value_at(float x) const {
    Rect t = track_rect();
    float x0 = t.x + 10, x1 = std::max(t.x + 11, t.right() - 10);
    float p = clampf((x - x0) / std::max(1.0f, x1 - x0), 0, 1);
    return snap(from_ + (to_ - from_) * p);
}

float Slider::x_of(double v) const {
    Rect t = track_rect();
    float x0 = t.x + 10, x1 = std::max(t.x + 11, t.right() - 10);
    double range = to_ - from_;
    double p = std::abs(range) < 1e-9 ? 0.0 : (v - from_) / range;
    p = p < 0 ? 0 : (p > 1 ? 1 : p);
    return x0 + (x1 - x0) * static_cast<float>(p);
}

std::string Slider::shown() const { return std::format("{:.{}f}{}", value_ * unit_.scale, unit_.decimals, unit_.suffix); }

void Slider::paint(Gfx& g) {
    grow_.set((hover_ || dragging_) ? 1.0f : 0.0f);
    animate(grow_, 24);
    float h = grow_.value;
    Rect t = track_rect();
    float cy = t.y + t.h / 2;
    Rect groove{t.x + 2, cy - 2.5f, t.w - 4, 5};
    g.fill_rrect(groove, 2.5f, TRACK);
    // wartosc zmieniona z zewnatrz (preset, reset) - uchwyt dojezdza; pod mysza idzie od razu
    double span = to_ - from_;
    float frac = std::abs(span) < 1e-9 ? 0.0f : clampf(static_cast<float>((value_ - from_) / span), 0, 1);
    if (dragging_ || !placed_) pos_.snap(frac);
    placed_ = true;
    pos_.set(frac);
    if (pos_.step(Ui::get().dt, 22.0f, 0.0015f)) invalidate();
    float x = x_of(from_ + span * pos_.value);
    if (x > groove.x + 2) g.fill_rrect_h({groove.x, groove.y, x - groove.x, groove.h}, 2.5f, ACCENT_DIM, ACCENT);
    if (default_) {
        float dx = x_of(*default_);
        g.fill_rect({std::round(dx), cy + 6, 1, 5}, MUTED, 0.8f);
    }
    if (h > 0.01f) g.fill_circle(x, cy, 10 + 4 * h, ACCENT, 0.18f * h);
    g.fill_circle(x, cy, 8 + h, ACCENT);
    g.fill_circle(x, cy, 4 + h, WHITE);

    Rect b = bubble_rect();
    bool focus = Ui::get().focus() == this && editing_;
    g.fill_rrect(b, 7, focus ? PANEL_HI : mix(PANEL_HI, ACCENT, 0.10));
    if (focus) g.stroke_rrect(b, 7, ACCENT);
    if (editing_) {
        g.text(b, edit_, Font::Num, TEXT, Align::Center);
    } else {
        g.text(b, shown(), Font::Num, ACCENT, Align::Center);
    }
    if (changed()) {
        Rect c = reset_rect();
        bool hot = c.contains(Ui::get().mouse.x, Ui::get().mouse.y);
        g.text(c, ic::undo, Font::IconSmall, hot ? TEXT : (hover_ ? TEXT_DIM : MUTED), Align::Center);
    }
}

void Slider::mouse_down(float x, float y) {
    if (bubble_rect().contains(x, y)) {
        editing_ = true;
        edit_.clear();
        Ui::get().set_focus(this);
        invalidate();
        return;
    }
    if (changed() && reset_rect().contains(x, y)) {
        apply(*default_);
        return;
    }
    if (!track_rect().contains(x, y)) return;
    Ui::get().set_focus(this);
    dragging_ = true;
    apply(value_at(x));
    invalidate();
}

void Slider::mouse_dbl(float x, float y) {
    // podwojne klikniecie na rowku wraca do wartosci domyslnej
    if (default_ && track_rect().contains(x, y)) {
        dragging_ = false;
        apply(*default_);
        return;
    }
    mouse_down(x, y);
}

void Slider::mouse_move(float x, float) {
    if (dragging_) apply(value_at(x));
}

void Slider::mouse_up(float, float) {
    dragging_ = false;
    invalidate();
}

bool Slider::key_down(unsigned vk) {
    if (editing_) {
        if (vk == VK_RETURN) {
            commit_edit();
            return true;
        }
        if (vk == VK_ESCAPE) {
            editing_ = false;
            Ui::get().set_focus(nullptr);
            invalidate();
            return true;
        }
        if (vk == VK_BACK && !edit_.empty()) {
            edit_.pop_back();
            invalidate();
            return true;
        }
        return false;
    }
    double step = resolution_ != 0.0 ? resolution_ : (to_ - from_) / 100.0;
    if (vk == VK_LEFT || vk == VK_DOWN) apply(snap(value_ - step));
    else if (vk == VK_RIGHT || vk == VK_UP) apply(snap(value_ + step));
    else if (vk == VK_HOME) apply(snap(std::min(from_, to_)));
    else if (vk == VK_END) apply(snap(std::max(from_, to_)));
    else return false;
    return true;
}

void Slider::char_input(wchar_t c) {
    if (!editing_) return;
    if ((c >= L'0' && c <= L'9') || c == L'.' || c == L',' || c == L'-') {
        edit_.push_back(c == L',' ? '.' : static_cast<char>(c));
        invalidate();
    }
}

void Slider::commit_edit() {
    if (!edit_.empty()) {
        char* end = nullptr;
        double v = std::strtod(edit_.c_str(), &end);
        if (end != edit_.c_str()) apply(snap(v / unit_.scale));
    }
    editing_ = false;
    Ui::get().set_focus(nullptr);
    invalidate();
}

void Slider::focus_changed(bool focused) {
    if (!focused && editing_) commit_edit();
    invalidate();
}

// -------------------------------------------------------------- wskazniki

void StatusDot::set(RGB color, bool glow) {
    if (color_ != color || glow_ != glow) {
        color_ = color;
        glow_ = glow;
        invalidate();
    }
}

void StatusDot::paint(Gfx& g) {
    float cx = r.x + size_ / 2, cy = r.y + size_ / 2;
    float rad = size_ / 2 - std::max(2.0f, std::floor(size_ / 5));
    if (glow_) g.fill_circle(cx, cy, size_ / 2, color_, 0.22f);
    g.fill_circle(cx, cy, rad, color_);
}

void StatRow::set(const std::string& value) {
    if (value_ != value) {
        value_ = value;
        invalidate();
    }
}

void StatRow::set(const std::string& value, RGB color) {
    set(value);
    if (color_ != color) {
        color_ = color;
        invalidate();
    }
}

Size StatRow::measure(float avail_w) { return {avail_w, 26}; }

void StatRow::paint(Gfx& g) {
    g.text(r, label_, Font::Small, MUTED);
    g.text(r, value_, Font::Num, color_, Align::Right);
}

StatTile::StatTile(std::string label, bool spark) : label_(upper_pl(label)), spark_(spark) {}

void StatTile::set(const std::string& value, RGB color) {
    if (value_ != value || color_ != color) {
        value_ = value;
        color_ = color;
        invalidate();
    }
}

void StatTile::push(double value, double scale_max) {
    data_.push_back(std::max(0.0, value));
    while (data_.size() > 90) data_.pop_front();
    top_ = std::max(scale_max, 1e-6);
    invalidate();
}

void StatTile::paint(Gfx& g) {
    g.fill_rrect_v(r, 12, 0x181e28, 0x131821);
    g.stroke_rrect(r, 12, BORDER);
    g.text({r.x + 14, r.y + 10, r.w - 20, 16}, label_, Font::Caps, MUTED);
    g.text({r.x + 14, r.y + 28, r.w - 24, 34}, value_, Font::NumLarge, color_);
    if (!spark_ || data_.size() < 2) return;
    Rect area{r.x + 1, r.bottom() - 44, r.w - 2, 43};
    float step = area.w / 89.0f;
    size_t offset = 90 - data_.size();
    std::vector<Pt> line;
    line.reserve(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) {
        float x = area.x + static_cast<float>(offset + i) * step;
        float y = area.bottom() - 4 - static_cast<float>(std::min(data_[i], top_) / top_) * (area.h - 10);
        line.push_back({x, y});
    }
    std::vector<Pt> fill;
    fill.reserve(line.size() + 2);
    fill.push_back({line.front().x, area.bottom()});
    fill.insert(fill.end(), line.begin(), line.end());
    fill.push_back({line.back().x, area.bottom()});
    g.push_rounded_clip(r, 12);
    g.fill_poly_v(fill, area.y, area.bottom(), color_, color_, 0.28f, 0.0f);
    g.polyline(line, color_, 1.8f);
    g.pop_rounded_clip();
}

void TorqueMeter::set(double value) {
    value = value != value ? 0.0 : std::max(-1.0, std::min(1.0, value));
    if (std::abs(value - value_) >= 0.002) {
        value_ = value;
        invalidate();
    }
}

void TorqueMeter::paint(Gfx& g) {
    float radius = r.h / 2;
    g.fill_rrect(r, radius, TRACK);
    float mid = r.x + r.w / 2;
    float len = static_cast<float>(std::abs(value_)) * r.w / 2;
    if (len >= 2) {
        RGB color = mix(VIOLET, BAD, std::max(0.0, std::abs(value_) - 0.6) / 0.4);
        Rect fill = value_ >= 0 ? Rect{mid, r.y + 2, len, r.h - 4} : Rect{mid - len, r.y + 2, len, r.h - 4};
        g.shadow(fill, radius - 2, 7, color, 0.3f);
        if (value_ >= 0) g.fill_rrect_h(fill, radius - 2, mix(color, BG, 0.4), color);
        else g.fill_rrect_h(fill, radius - 2, color, mix(color, BG, 0.4));
    }
    for (float f : {0.25f, 0.75f}) {
        float x = std::round(r.x + r.w * f);
        g.line(x + 0.5f, r.y + r.h * 0.3f, x + 0.5f, r.y + r.h * 0.7f, BORDER_HI);
    }
    g.fill_rect({std::round(mid), r.y, 1, r.h}, BORDER_HI);
    g.text(r, std::format("{:+.0f} %", value_ * 100), Font::Num, std::abs(value_) > 0.02 ? TEXT : MUTED, Align::Center);
}

void PedalBar::set(double value, std::optional<int> raw, bool enabled) {
    value = value != value ? 0.0 : (enabled ? std::max(0.0, std::min(1.0, value)) : 0.0);
    if (!enabled) raw.reset();
    if (std::abs(value - value_) > 0.0005 || raw != raw_ || enabled != enabled_) {
        value_ = value;
        raw_ = raw;
        enabled_ = enabled;
        invalidate();
    }
}

Size PedalBar::measure(float) { return {width_, 20 + bar_h_ + 54}; }

void PedalBar::paint(Gfx& g) {
    g.text({r.x - 8, r.y, r.w + 16, 16}, label_, Font::SmallStrong, enabled_ ? TEXT_DIM : MUTED, Align::Center);
    Rect bar{r.x, r.y + 22, r.w, bar_h_};
    float radius = std::min(12.0f, r.w / 4);
    g.fill_rrect_v(bar, radius, 0x1c232e, 0x171d27);
    g.stroke_rrect(bar, radius, BORDER);
    for (float f : {0.25f, 0.5f, 0.75f}) {
        float y = std::round(bar.y + bar.h * (1.0f - f));
        g.fill_rect({bar.x + 8, y, bar.w - 16, 1}, BORDER_HI, 0.7f);
    }
    float h = std::round(bar.h * static_cast<float>(value_));
    if (h >= 1) {
        Rect fill{bar.x, bar.bottom() - h, bar.w, h};
        g.shadow(fill, radius, 12, color_, 0.25f);
        g.push_rounded_clip(bar, radius);
        g.fill_rrect_v({fill.x, fill.y, fill.w, std::max(fill.h, radius * 2)}, 0, lighten(color_, 0.22), mix(color_, BG, 0.25));
        g.fill_rect({fill.x, fill.y, fill.w, 2}, WHITE, 0.55f);
        g.pop_rounded_clip();
    }
    float y = bar.bottom() + 6;
    bool big = r.w >= 56;
    Font font = big ? Font::NumLarge : Font::Num;
    float pct_h = big ? 30.0f : 22.0f;
    if (enabled_) g.text({r.x - 10, y, r.w + 20, pct_h}, std::format("{:.0f}%", value_ * 100), font, value_ > 0.002 ? TEXT : MUTED, Align::Center);
    else g.text({r.x - 10, y, r.w + 20, pct_h}, "wył.", font, MUTED, Align::Center);
    if (raw_) g.text({r.x - 10, y + pct_h, r.w + 20, 16}, std::to_string(*raw_), Font::MonoSmall, MUTED, Align::Center);
}

void ButtonGrid::set_mask(uint32_t mask) {
    if (mask_ != mask) {
        mask_ = mask;
        invalidate();
    }
}

Size ButtonGrid::measure(float avail_w) { return {avail_w > 0 ? avail_w : 320, 4 * 26 + 3 * 5}; }

void ButtonGrid::paint(Gfx& g) {
    const int cols = 8;
    float gap = 5, step = (r.w - gap * (cols - 1)) / cols;
    for (int i = 0; i < 32; ++i) {
        int col = i % cols, row = i / cols;
        float x1 = std::round(static_cast<float>(col) * (step + gap));
        float x2 = std::round(static_cast<float>(col) * (step + gap) + step);
        Rect cell{r.x + x1, r.y + static_cast<float>(row) * 31, std::max(4.0f, x2 - x1), 26};
        bool on = (mask_ >> i) & 1;
        if (on) g.shadow(cell, 7, 7, ACCENT, 0.45f);
        g.fill_rrect(cell, 7, on ? ACCENT : TRACK);
        g.text(cell, std::to_string(i), Font::MonoSmall, on ? ACCENT_INK : MUTED, Align::Center);
    }
}

void HatIndicator::set(int direction) {
    if (dir_ != direction) {
        dir_ = direction;
        invalidate();
    }
}

void HatIndicator::paint(Gfx& g) {
    float s = size_;
    int dir = (dir_ >= 0 && dir_ < 4) ? dir_ : -1;
    const Sprite& sprite = g.cached("hat/" + std::to_string(static_cast<int>(s)) + "/" + std::to_string(dir), [&] {
        float c = s / 2, gr = s * 0.12f, b = s * 0.17f, w = s * 0.13f, o = s * 0.42f;
        Paint p(static_cast<int>(s), static_cast<int>(s));
        p.gradient(Shape::circle(c, c, c - 2), 0x1b212b, 0x12161d);
        p.stroke(Shape::circle(c, c, c - 2), BORDER, 1.0f);
        std::vector<Pt> arrows[4] = {{{c, c - o}, {c - w, c - b}, {c + w, c - b}},
                                     {{c + o, c}, {c + b, c - w}, {c + b, c + w}},
                                     {{c, c + o}, {c - w, c + b}, {c + w, c + b}},
                                     {{c - o, c}, {c - b, c - w}, {c - b, c + w}}};
        for (int i = 0; i < 4; ++i) {
            bool on = i == dir;
            if (on) p.glow(Shape::poly(arrows[i]), ACCENT, 4, 120);
            p.fill(Shape::poly(arrows[i]), on ? ACCENT : BORDER_HI);
        }
        p.fill(Shape::circle(c, c, gr * 0.75f), BORDER);
        p.fill(Shape::circle(c, c, gr * 0.75f - 1), PANEL_HI);
        return p.finish();
    });
    g.draw_sprite(sprite, r.x, r.y);
}

// ------------------------------------------------------------ CurveEditor

CurveEditor::CurveEditor(float height, RGB color, bool bipolar, std::array<std::string, 3> captions, Handlers handlers)
    : height_(height), color_(color), bipolar_(bipolar), captions_(std::move(captions)), h_(std::move(handlers)) {}

void CurveEditor::set_curve(std::function<double(double)> fn) { fn_ = std::move(fn); }

void CurveEditor::set_params(double dz_low, double dz_high, double gamma) {
    if (dz_low != dz_low_ || dz_high != dz_high_ || gamma != gamma_) {
        dz_low_ = dz_low;
        dz_high_ = dz_high;
        gamma_ = gamma;
        invalidate();
    }
}

void CurveEditor::set_marker(std::optional<std::pair<double, double>> xy) {
    if (xy) {
        double lo = bipolar_ ? -1.0 : 0.0;
        xy->first = std::min(1.0, std::max(lo, xy->first));
        xy->second = std::min(1.0, std::max(lo, xy->second));
    }
    if (xy != marker_) {
        marker_ = xy;
        invalidate();
    }
}

void CurveEditor::set_captions(const std::string& left, const std::string& middle, const std::string& right) {
    if (captions_[0] != left || captions_[1] != middle || captions_[2] != right) {
        captions_ = {left, middle, right};
        invalidate();
    }
}

void CurveEditor::set_color(RGB color) {
    if (color_ != color) {
        color_ = color;
        invalidate();
    }
}

Pt CurveEditor::to_px(double x, double y) const {
    double lo = bipolar_ ? -1.0 : 0.0;
    const float inset = 12;
    float pw = r.w - 2 * inset, ph = height_ - 2 * inset;
    double tx = (x - lo) / (1.0 - lo), ty = (y - lo) / (1.0 - lo);
    return {r.x + inset + static_cast<float>(tx) * pw, r.y + inset + static_cast<float>(1.0 - ty) * ph};
}

CurveEditor::Part CurveEditor::part_at(float x, float y) const {
    if (y > r.y + height_) return None;
    if (h_.low && std::abs(x - to_px(dz_low_, 0).x) <= 8) return Low;
    if (h_.high && std::abs(x - to_px(1.0 - dz_high_, 0).x) <= 8) return High;
    return h_.gamma ? Curve : None;
}

void CurveEditor::drag_to(float x, float y) {
    double lo = bipolar_ ? -1.0 : 0.0;
    const float inset = 12;
    float pw = r.w - 2 * inset, ph = height_ - 2 * inset;
    double nx = (x - r.x - inset) / pw * (1.0 - lo) + lo;
    double ny = (1.0 - (y - r.y - inset) / ph) * (1.0 - lo) + lo;
    if (drag_ == Low && h_.low) {
        h_.low(std::rint(std::max(0.0, std::min(0.45, nx)) * 100) / 100);
    } else if (drag_ == High && h_.high) {
        h_.high(std::rint(std::max(0.0, std::min(0.45, 1.0 - nx)) * 100) / 100);
    } else if (drag_ == Curve && h_.gamma) {
        // punkt pod kursorem wyznacza wykladnik: y = x^g
        double span = 1.0 - dz_low_ - dz_high_;
        double t = bipolar_ ? std::abs(nx) : (span > 1e-6 ? (nx - dz_low_) / span : nx);
        double out = bipolar_ ? std::abs(ny) : ny;
        t = std::max(0.08, std::min(0.92, t));
        out = std::max(0.03, std::min(0.97, out));
        h_.gamma(std::log(out) / std::log(t));
    }
}

void CurveEditor::paint(Gfx& g) {
    Rect field{r.x, r.y, r.w, height_};
    g.fill_rrect_v(field, 10, 0x171d27, 0x141923);
    g.stroke_rrect(field, 10, BORDER);
    double lo = bipolar_ ? -1.0 : 0.0;

    // martwe strefy - pas na cala wysokosc pola
    if (h_.low || h_.high) {
        for (auto [a, b] : {std::pair{0.0, dz_low_}, std::pair{1.0 - dz_high_, 1.0}}) {
            if (b - a < 0.002) continue;
            float x1 = std::max(r.x + 2, to_px(a, 0).x), x2 = std::min(r.right() - 2, to_px(b, 0).x);
            if (a <= 0) x1 = r.x + 2;
            if (b >= 1) x2 = r.right() - 2;
            if (x2 > x1) g.fill_rect({x1, r.y + 2, x2 - x1, height_ - 4}, BAD, 0.10f);
        }
    }

    RGB grid = mix(PANEL_HI, BORDER_HI, 0.55);
    for (double f : {0.25, 0.5, 0.75}) {
        double v = lo + (1.0 - lo) * f;
        Pt p = to_px(v, v);
        RGB color = (bipolar_ && f == 0.5) ? BORDER_HI : grid;
        g.line(std::round(p.x) + 0.5f, r.y + 3, std::round(p.x) + 0.5f, r.y + height_ - 3, color, 1.0f, 0.8f);
        g.line(r.x + 3, std::round(p.y) + 0.5f, r.right() - 3, std::round(p.y) + 0.5f, color, 1.0f, 0.8f);
    }
    // przekatna - tak wyglada odpowiedz liniowa
    {
        Pt a = to_px(lo, lo), b = to_px(1.0, 1.0);
        g.line(a.x, a.y, b.x, b.y, TEXT_DIM, 1.0f, 0.25f);
    }

    if (fn_) {
        const int samples = 140;
        std::vector<Pt> pts;
        pts.reserve(samples + 1);
        for (int i = 0; i <= samples; ++i) {
            double x = lo + (1.0 - lo) * i / samples;
            pts.push_back(to_px(x, fn_(x)));
        }
        float base_y = to_px(0, 0).y;
        std::vector<Pt> area;
        area.reserve(pts.size() + 2);
        area.push_back({pts.front().x, base_y});
        area.insert(area.end(), pts.begin(), pts.end());
        area.push_back({pts.back().x, base_y});
        g.fill_poly_v(area, r.y, r.y + height_, color_, color_, 0.22f, 0.02f);
        bool hot = hover_ == Curve || drag_ == Curve;
        g.polyline(pts, color_, 7, hot ? 0.28f : 0.14f);
        g.polyline(pts, color_, 2.4f);
    }

    // uchwyty martwych stref
    auto handle = [&](double value, Part part) {
        Pt p = to_px(value, 0);
        bool hot = hover_ == part || drag_ == part;
        float x = std::round(p.x);
        g.line(x, r.y + 6, x, r.y + height_ - 6, hot ? BAD : mix(BAD, PANEL_HI, 0.35), hot ? 2.0f : 1.4f);
        Rect grip{x - 5, r.y + height_ / 2 - 15, 10, 30};
        g.fill_rrect(grip, 5, hot ? BAD : mix(BAD, PANEL_HI, 0.2));
        for (float dy : {-5.0f, 0.0f, 5.0f}) g.fill_rect({x - 2, r.y + height_ / 2 + dy, 4, 1}, WHITE, 0.7f);
    };
    if (h_.low) handle(dz_low_, Low);
    if (h_.high) handle(1.0 - dz_high_, High);

    if (h_.gamma) {
        // obok uchwytu martwej strefy, nie pod nim
        float gx = h_.low ? to_px(dz_low_, 0).x + 12 : r.x + 12;
        g.text({gx, r.y + 8, 120, 18}, std::format("γ {:.2f}", gamma_), Font::Num, TEXT_DIM);
    }

    Rect caption{r.x + 2, r.y + height_ + 3, r.w - 4, 16};
    g.text(caption, captions_[0], Font::Tiny, MUTED);
    g.text(caption, captions_[1], Font::Tiny, MUTED, Align::Center);
    g.text(caption, captions_[2], Font::Tiny, MUTED, Align::Right);

    if (marker_) {
        Pt m = to_px(marker_->first, marker_->second);
        g.fill_circle(m.x, m.y, 10, color_, 0.25f);
        g.fill_circle(m.x, m.y, 5.5f, color_);
        g.fill_circle(m.x, m.y, 3, WHITE);
    }
}

void CurveEditor::mouse_move(float x, float y) {
    if (drag_ != None) {
        drag_to(x, y);
        return;
    }
    Part p = part_at(x, y);
    if (p != hover_) {
        hover_ = p;
        invalidate();
    }
}

void CurveEditor::mouse_leave() {
    if (hover_ != None) {
        hover_ = None;
        invalidate();
    }
}

void CurveEditor::mouse_down(float x, float y) {
    drag_ = part_at(x, y);
    if (drag_ != None) {
        Ui::get().set_focus(nullptr);
        drag_to(x, y);
    }
    invalidate();
}

void CurveEditor::mouse_up(float, float) {
    drag_ = None;
    invalidate();
}

// --------------------------------------------------------------- RangeBar

void RangeBar::set(int cal_min, int cal_max, std::optional<int> raw, std::optional<std::pair<int, int>> seen) {
    if (cal_min != cal_min_ || cal_max != cal_max_ || raw != raw_ || seen != seen_) {
        cal_min_ = cal_min;
        cal_max_ = cal_max;
        raw_ = raw;
        seen_ = seen;
        invalidate();
    }
}

void RangeBar::set_color(RGB color) {
    if (color_ != color) {
        color_ = color;
        invalidate();
    }
}

void RangeBar::paint(Gfx& g) {
    auto x_of = [&](int v) { return r.x + r.w * static_cast<float>(std::clamp(v, 0, 4095)) / 4095.0f; };
    Rect bar{r.x, r.y + 8, r.w, 10};
    g.fill_rrect(bar, 5, TRACK);
    float x1 = x_of(cal_min_), x2 = x_of(cal_max_);
    g.fill_rrect({x1, bar.y, std::max(2.0f, x2 - x1), bar.h}, 5, color_, 0.35f);
    if (seen_) {
        float a = x_of(seen_->first), b = x_of(seen_->second);
        g.fill_rect({a, bar.y - 5, std::max(1.0f, b - a), 2}, TEXT_DIM, 0.8f);
    }
    if (raw_) {
        float x = std::round(x_of(*raw_));
        g.fill_rect({x - 1, bar.y - 3, 2, bar.h + 6}, WHITE);
    }
    Rect caption{r.x, bar.bottom() + 6, r.w, 16};
    g.text(caption, std::format("kalibracja {} – {}", cal_min_, cal_max_), Font::Tiny, MUTED);
    if (seen_) g.text(caption, std::format("widziano {} – {}", seen_->first, seen_->second), Font::Tiny, MUTED, Align::Center);
    g.text(caption, raw_ ? std::to_string(*raw_) : std::string("-"), Font::MonoSmall, TEXT_DIM, Align::Right);
}

// -------------------------------------------------------------- WheelDial

void WheelDial::set_range(double degrees) {
    degrees = std::max(1.0, degrees);
    if (!placed_) {
        // pierwsza wartosc bez animacji
        placed_ = true;
        range_ = degrees;
        shown_.snap(static_cast<float>(degrees));
        invalidate();
    } else if (std::abs(degrees - range_) >= 0.5) {
        range_ = degrees;
        shown_.set(static_cast<float>(degrees));
        flash_.snap(1.0f);
        flash_.set(0.0f);
        invalidate();
    }
}

void WheelDial::set_angle(double degrees) {
    if (degrees != degrees) degrees = 0;
    if (std::abs(degrees - deg_) >= 0.2) {
        deg_ = degrees;
        invalidate();
    }
}

Sprite WheelDial::wheel_sprite(RGB marker) {
    // Obraca sie razem z kierownica, wiec bez swiatla z gory - to doklada
    // nieruchoma nakladka z light_sprite().
    float s = size_;
    float rim_w = std::max(13.0f, s / 15.5f);
    float rim_o = s / 2 - 34, rim_i = rim_o - rim_w, rim_r = rim_o - rim_w / 2;
    int half = static_cast<int>(std::ceil(rim_o)) + 10;
    Paint p(half * 2, half * 2);
    float c = static_cast<float>(half);
    Shape rim = Shape::ring(c, c, rim_o, rim_i, 0, 360);
    p.shadow(rim, 0, 6, 190);

    // ramiona: sfazowane aluminium z wyfrezowanym oknem
    float hub = std::max(22.0f, s / 8.5f);
    for (float deg : {0.0f, 180.0f, 90.0f}) {
        float a = deg * PI / 180.0f;
        float ca = std::cos(a), sa = std::sin(a);
        float nx = -sa, ny = ca;
        float w0 = hub * 0.78f, w1 = rim_w * 0.82f;
        float r0 = hub * 0.5f, r1 = rim_i + 3;
        // czworokat wzdluz ramienia: od ulamka t0 do t1 dlugosci, k - czesc szerokosci
        auto quad = [&](float t0, float t1, float k) {
            float ra = r0 + (r1 - r0) * t0, rb = r0 + (r1 - r0) * t1;
            float wa = (w0 + (w1 - w0) * t0) * k, wb = (w0 + (w1 - w0) * t1) * k;
            return Shape::poly({{c + ca * ra + nx * wa, c + sa * ra + ny * wa}, {c + ca * rb + nx * wb, c + sa * rb + ny * wb},
                                {c + ca * rb - nx * wb, c + sa * rb - ny * wb}, {c + ca * ra - nx * wa, c + sa * ra - ny * wa}});
        };
        p.fill(quad(0, 1, 1), 0x262c36);
        p.fill(quad(0, 1, 0.78f), 0x465061);
        p.stroke(quad(0, 1, 1), WHITE, 1.0f, 26);
        Shape window = quad(0.50f, 0.86f, 0.42f);
        p.fill(window, 0x0d1015);
        p.stroke(window, BLACK, 1.0f, 150);
    }

    // obrecz: skora, po bokach perforowane uchwyty
    p.fill(rim, 0x2b313b);
    for (float ctr : {0.0f, 180.0f}) {
        Shape grip = Shape::ring(c, c, rim_o - 1, rim_i + 1, ctr - 42, ctr + 42);
        p.fill(grip, 0x191d24);
        int row = 0;
        for (float rr = rim_i + 4; rr <= rim_o - 3.5f; rr += 3.4f, ++row)
            for (float deg = ctr - 39 + (row % 2 ? 1.6f : 0.0f); deg <= ctr + 39; deg += 3.2f) {
                float a = deg * PI / 180.0f;
                p.fill(Shape::circle(c + rr * std::cos(a), c + rr * std::sin(a), 0.75f), BLACK, 130);
            }
        for (float deg : {ctr - 42, ctr + 42}) {
            float a = deg * PI / 180.0f;
            p.line({{c + (rim_i + 1.5f) * std::cos(a), c + (rim_i + 1.5f) * std::sin(a)}, {c + (rim_o - 1.5f) * std::cos(a), c + (rim_o - 1.5f) * std::sin(a)}},
                   0x9aa5b8, 1.2f, 150, false);
        }
    }
    p.stroke(Shape::circle(c, c, rim_o), WHITE, 1.0f, 26);
    p.arc(c, c, rim_i + 0.8f, 0, 360, BLACK, 1.6f, 110);
    // szew na gorze i na dole, miedzy uchwytami
    for (int k = 0; k < 96; ++k) {
        float deg = static_cast<float>(k) * 3.75f;
        float off = std::fmod(deg + 44.0f, 180.0f);
        if (off < 88.0f) continue;  // pod uchwytem
        float a = deg * PI / 180.0f, b = (deg + 1.7f) * PI / 180.0f;
        p.line({{c + rim_r * std::cos(a), c + rim_r * std::sin(a)}, {c + rim_r * std::cos(b), c + rim_r * std::sin(b)}}, WHITE, 0.9f, 44);
    }

    // piasta ze srubami - po nich widac obrot takze w srodku
    Shape boss = Shape::circle(c, c, hub);
    p.shadow(boss, 2, 5, 180);
    p.fill(boss, 0x2a303a);
    p.stroke(boss, WHITE, 1.0f, 24);
    for (int k = 0; k < 6; ++k) {
        float a = (static_cast<float>(k) * 60.0f + 30.0f) * PI / 180.0f;
        Shape bolt = Shape::circle(c + hub * 0.80f * std::cos(a), c + hub * 0.80f * std::sin(a), std::max(1.6f, hub * 0.075f));
        p.fill(bolt, 0x8a94a6);
        p.stroke(bolt, BLACK, 0.8f, 170);
    }

    // pasek "gora kierownicy" na obreczy
    p.glow(Shape::rrect(c - 3.5f, c - rim_o + 1, c + 3.5f, c - rim_i - 1, 1), marker, 4, 110);
    p.line({{c, c - rim_o + 1}, {c, c - rim_i - 1}}, marker, 7, 255, false);
    p.line({{c - 1.5f, c - rim_o + 2}, {c - 1.5f, c - rim_i - 2}}, WHITE, 1.0f, 90, false);
    return p.finish();
}

// Swiatlo z gory i srodek piasty - nie obracaja sie z kierownica.
Sprite WheelDial::light_sprite(RGB marker) {
    float s = size_, c = s / 2;
    float rim_w = std::max(13.0f, s / 15.5f);
    float rim_o = s / 2 - 34, rim_i = rim_o - rim_w;
    float hub = std::max(22.0f, s / 8.5f);
    Paint p(static_cast<int>(s), static_cast<int>(s));
    Shape rim = Shape::ring(c, c, rim_o, rim_i, 0, 360);
    p.sheen(rim, 62, 0.42f);
    p.arc(c, c, rim_o - 1.2f, 215, 325, WHITE, 1.2f, 46);
    p.sheen(Shape::circle(c, c, hub), 44, 0.55f);
    Shape cap = Shape::circle(c, c, hub * 0.60f);
    p.glow(cap, marker, 4, 70);
    p.gradient(cap, 0x1c222c, 0x0e1116);
    p.stroke(cap, mix(ACCENT_DIM, marker, 0.7), 2.0f);
    p.stroke(Shape::circle(c, c, hub * 0.34f), WHITE, 1.0f, 26);
    p.fill(Shape::circle(c, c, hub * 0.13f), marker);
    return p.finish();
}

void WheelDial::paint(Gfx& g) {
    float s = size_;
    float c = s / 2;
    float cx = r.x + c, cy = r.y + c;
    const float band_c = c - 17;  // srodek pasa, po ktorym biegnie tor

    if (shown_.step(Ui::get().dt, 9.0f)) invalidate();
    animate(flash_, 2.6f);
    float fl = flash_.value;

    // Tor jest w prawdziwych stopniach kierownicy: 270 to luk, 900 to dwa i
    // pol zwoju spirali. Zmiana zakresu nawija go albo odwija.
    float range = std::max(1.0f, shown_.value);
    float half = range / 2.0f;
    float turns = range / 360.0f;
    float pitch = std::min(10.0f, 20.0f / turns) * smoothstep(0.80f, 0.98f, turns);  // odstep zwojow [px]
    float lane = turns >= 1.0f ? clampf(pitch - 3.0f, 2.0f, 4.5f) : 5.5f - smoothstep(0.8f, 1.0f, turns);
    auto spot = [&](float deg, float off = 0.0f) {
        float a = (deg - 90.0f) * PI / 180.0f;
        float rad = band_c + pitch * deg / 360.0f + off;
        return Pt{cx + rad * std::cos(a), cy + rad * std::sin(a)};
    };
    auto path = [&](float from, float to) {
        if (from > to) std::swap(from, to);
        int steps = std::max(2, static_cast<int>((to - from) / 3.0f) + 1);
        std::vector<Pt> pts;
        pts.reserve(static_cast<size_t>(steps) + 1);
        for (int i = 0; i <= steps; ++i) pts.push_back(spot(from + (to - from) * static_cast<float>(i) / static_cast<float>(steps)));
        return pts;
    };

    // tarcza: siatka katow co 30 st. i znacznik srodka
    const Sprite& base = g.cached("dial/" + std::to_string(static_cast<int>(s)), [&] {
        Paint p(static_cast<int>(s), static_cast<int>(s));
        Shape face = Shape::circle(c, c, c - 1);
        p.gradient(face, 0x151a23, 0x0b0e13);
        p.stroke(face, WHITE, 1.0f, 14);
        for (int k = 0; k < 12; ++k) {
            float a = static_cast<float>(k) * 30.0f * PI / 180.0f;
            bool major = k % 3 == 0;
            float r0 = band_c - 12, r1 = band_c + 12;
            p.line({{c + r0 * std::cos(a), c + r0 * std::sin(a)}, {c + r1 * std::cos(a), c + r1 * std::sin(a)}}, major ? BORDER_HI : BORDER, 1.0f, major ? 190 : 150, false);
        }
        p.fill(Shape::poly({{c, 6.5f}, {c - 4.5f, 0.5f}, {c + 4.5f, 0.5f}}), TEXT_DIM);
        return p.finish();
    });
    g.draw_sprite(base, r.x, r.y);

    float pos = clampf(static_cast<float>(deg_), -half, half);
    bool over = std::abs(deg_) > range_ / 2.0 + 0.5;
    double near_end = std::rint(std::min(1.0, std::max(0.0, (std::abs(pos) / half - 0.75) / 0.25)) * 4) / 4;
    RGB color = over ? BAD : mix(ACCENT, WARN, near_end);

    std::vector<Pt> track = path(-half, half);
    if (fl > 0.01f) g.polyline(track, ACCENT, lane + 7, 0.16f * fl);
    g.polyline(track, mix(TRACK, 0x34404f, 0.5), lane);

    // konce toru swieca, dopoki zakres sie zmienia
    if (fl > 0.01f)
        for (float side : {-1.0f, 1.0f}) {
            float tail = std::min(half, 26.0f + 60.0f * fl);
            g.polyline(path(side * (half - tail), side * half), ACCENT_HI, lane, 0.85f * fl);
        }

    if (std::abs(pos) > 0.4f) {
        std::vector<Pt> arc = path(0, pos);
        g.polyline(arc, color, lane + 8, 0.14f);
        g.polyline(arc, color, lane);
    }

    // opory: konce toru, podpisy w rogach
    RGB stop = over ? BAD : mix(TEXT_DIM, ACCENT_HI, fl);
    for (float side : {-1.0f, 1.0f}) {
        Pt a = spot(side * half, -lane / 2 - 2.5f), b = spot(side * half, lane / 2 + 2.5f);
        g.line(a.x, a.y, b.x, b.y, stop, 2.4f);
        g.text_at(side < 0 ? r.x + 26 : r.right() - 26, r.bottom() - 9, std::format("{}{:.0f}°", side < 0 ? "−" : "+", half), Font::TinyStrong, over ? BAD : mix(MUTED, ACCENT_HI, fl));
    }

    Pt head = spot(pos);
    g.fill_circle(head.x, head.y, 10, color, 0.20f);
    g.fill_circle(head.x, head.y, 6, color);
    g.fill_circle(head.x, head.y, 2.5f, WHITE);

    const Sprite& wheel = g.cached(std::format("dial-wheel/{}/{:06x}", static_cast<int>(s), color), [&] { return wheel_sprite(color); });
    g.draw_sprite_rotated(wheel, cx, cy, static_cast<float>(deg_));
    const Sprite& light = g.cached(std::format("dial-light/{}/{:06x}", static_cast<int>(s), color), [&] { return light_sprite(color); });
    g.draw_sprite(light, r.x, r.y);
}

// ------------------------------------------------------------- RangeRuler

void RangeRuler::set_range(double degrees) {
    degrees = std::max(1.0, degrees);
    if (!placed_) {
        placed_ = true;
        range_ = degrees;
        shown_.snap(static_cast<float>(degrees));
        invalidate();
    } else if (std::abs(degrees - range_) >= 0.5) {
        range_ = degrees;
        shown_.set(static_cast<float>(degrees));
        flash_.snap(1.0f);
        flash_.set(0.0f);
        invalidate();
    }
}

void RangeRuler::set_angle(std::optional<double> degrees) {
    if (degrees && *degrees != *degrees) degrees = 0.0;
    if (degrees.has_value() != deg_.has_value() || (degrees && std::abs(*degrees - *deg_) >= 0.2)) {
        deg_ = degrees;
        invalidate();
    }
}

void RangeRuler::paint(Gfx& g) {
    if (shown_.step(Ui::get().dt)) invalidate();
    animate(flash_, 3.2f);
    float fl = flash_.value;

    Rect field{r.x, r.y, r.w, 50};
    if (fl > 0.01f) g.shadow(field, 10, 9, ACCENT, 0.22f * fl);
    g.fill_rrect_v(field, 10, 0x171d27, 0x141923);
    g.stroke_rrect(field, 10, mix(BORDER, ACCENT, fl * 0.8));

    float half = std::max(0.5f, shown_.value / 2.0f);
    float mid = std::round(field.x + field.w / 2);
    float span = std::max(20.0f, field.w / 2 - 16);
    float per_deg = span / half;
    float top = field.y + 7, label_y = field.y + 29;

    // kreski na stalych katach: zmiana zakresu przesuwa je i wymienia podpisy
    int last = static_cast<int>(half / 10.0f) * 10;
    for (int d = -last; d <= last; d += 10) {
        float spacing = static_cast<float>(d == 0 ? 360 : tick_step(std::abs(d))) * per_deg;  // odstep sasiadow [px]
        float edge = (half - static_cast<float>(std::abs(d))) * per_deg;
        float alpha = smoothstep(5.0f, 10.0f, spacing) * clampf(edge / 8.0f, 0, 1);
        if (alpha < 0.02f) continue;
        float big = smoothstep(26.0f, 60.0f, spacing);
        float x = std::round(mid + static_cast<float>(d) * per_deg) + 0.5f;
        g.line(x, top, x, top + 4 + 8 * big, mix(BORDER_HI, TEXT_DIM, big), 1.0f, alpha);
        float label = smoothstep(52.0f, 84.0f, spacing) * alpha * clampf((edge - 48.0f) / 16.0f, 0, 1);
        if (label > 0.02f) g.text_at(x, label_y, d == 0 ? std::string("0°") : std::format("{:+d}°", d), Font::Tiny, MUTED, label);
    }

    // opory
    bool over = deg_ && std::abs(*deg_) > range_ / 2.0 + 0.5;
    RGB stop = over ? BAD : mix(TEXT_DIM, ACCENT_HI, fl);
    for (float side : {-1.0f, 1.0f}) {
        float x = std::round(mid + side * span);
        g.fill_rect({x - 1, top, 2, 14}, stop);
    }
    Rect labels{field.x + 12, label_y - 8, field.w - 24, 16};
    g.text(labels, std::format("−{:.0f}°", half), Font::TinyStrong, stop);
    g.text(labels, std::format("+{:.0f}°", half), Font::TinyStrong, stop, Align::Right);

    // gdzie jest kierownica
    Rect track{mid - span, field.bottom() - 9, span * 2, 3};
    g.fill_rrect(track, 1.5f, TRACK);
    g.fill_rect({mid - 0.5f, track.y - 2, 1, track.h + 4}, BORDER_HI);
    if (deg_) {
        float frac = clampf(static_cast<float>(*deg_) / half, -1, 1);
        float x = mid + frac * span;
        RGB color = over ? BAD : ACCENT;
        if (std::abs(x - mid) >= 1) g.fill_rrect({std::min(mid, x), track.y, std::abs(x - mid), track.h}, 1.5f, color);
        float cy = track.y + track.h / 2;
        g.fill_circle(x, cy, 8, color, 0.22f);
        g.fill_circle(x, cy, 4.5f, color);
        g.fill_circle(x, cy, 2, WHITE);
    }
}

// ------------------------------------------------------------- TraceChart

TraceChart::TraceChart(std::vector<Series> series, std::vector<Lane> lanes, float height, double window_s)
    : series_(std::move(series)), lanes_(std::move(lanes)), height_(height), window_(window_s) {
    shown_.fill(true);
}

void TraceChart::set_live(bool live) {
    if (live_ != live) {
        live_ = live;
        invalidate();
    }
}

void TraceChart::push(double now, const std::array<float, MAX_SERIES>& values, bool repaint) {
    if (!samples_.empty() && now - samples_.back().t < 1.0 / 90.0) return;
    samples_.push_back({now, values});
    while (!samples_.empty() && now - samples_.front().t > window_ + 0.5) samples_.pop_front();
    now_ = now;
    if (repaint && live_) invalidate();
}

float TraceChart::legend_x(size_t index) const {
    Gfx& g = Gfx::get();
    float x = r.right();
    for (size_t i = series_.size(); i-- > index;) x -= g.measure(series_[i].name, Font::Tiny).width + 14 + 16;
    return x;
}

int TraceChart::legend_at(float x, float y) const {
    if (y > r.y + 20) return -1;
    Gfx& g = Gfx::get();
    for (size_t i = 0; i < series_.size(); ++i) {
        float left = legend_x(i);
        float w = g.measure(series_[i].name, Font::Tiny).width + 14;
        if (x >= left && x < left + w) return static_cast<int>(i);
    }
    return -1;
}

void TraceChart::paint(Gfx& g) {
    // legenda - klik chowa serie
    for (size_t i = 0; i < series_.size(); ++i) {
        float x = legend_x(i);
        float w = g.measure(series_[i].name, Font::Tiny).width + 14;
        bool on = shown_[i];
        g.fill_circle(x + 4, r.y + 9, 3.5f, series_[i].color, on ? 1.0f : 0.25f);
        g.text({x + 12, r.y + 1, w, 16}, series_[i].name, Font::Tiny, static_cast<int>(i) == hover_ ? TEXT : (on ? TEXT_DIM : MUTED));
    }

    float top = r.y + 24;
    float total_h = r.h - 24;
    float weights = 0;
    for (const Lane& lane : lanes_) weights += lane.weight;
    float gap = 8;
    float avail = total_h - gap * static_cast<float>(lanes_.size() - 1);
    float y = top;

    for (size_t l = 0; l < lanes_.size(); ++l) {
        const Lane& lane = lanes_[l];
        float h = std::round(avail * lane.weight / weights);
        Rect field{r.x, y, r.w, h};
        g.fill_rrect_v(field, 9, 0x171d27, 0x141923);
        g.stroke_rrect(field, 9, BORDER);
        g.push_rounded_clip(field, 9);

        // znaczniki czasu co 2 s przesuwaja sie z wykresem
        double phase = std::fmod(now_, 2.0);
        for (double t = phase; t < window_; t += 2.0) {
            float x = std::round(field.right() - static_cast<float>(t / window_) * field.w);
            g.fill_rect({x, field.y, 1, field.h}, BORDER, 0.7f);
        }
        auto lane_y = [&](float v) {
            if (lane.bipolar) return field.y + field.h / 2 - v * (field.h / 2 - 5);
            return field.bottom() - 4 - v * (field.h - 9);
        };
        if (lane.bipolar) g.fill_rect({field.x, std::round(field.y + field.h / 2), field.w, 1}, BORDER_HI, 0.9f);
        else g.fill_rect({field.x, std::round(field.y + field.h / 2), field.w, 1}, BORDER, 0.7f);

        for (size_t s = 0; s < series_.size(); ++s) {
            if (series_[s].lane != static_cast<int>(l) || !shown_[s] || samples_.size() < 2) continue;
            std::vector<Pt> pts;
            pts.reserve(samples_.size());
            float last_x = 1e9f;
            for (auto it = samples_.rbegin(); it != samples_.rend(); ++it) {
                double age = now_ - it->t;
                if (age > window_) break;
                float x = field.right() - static_cast<float>(age / window_) * field.w;
                if (last_x - x < 1.2f && !pts.empty()) continue;
                last_x = x;
                pts.push_back({x, lane_y(it->v[s])});
            }
            if (pts.size() < 2) continue;
            if (series_[s].area) {
                std::vector<Pt> area = pts;
                float base = lane_y(0);
                area.push_back({pts.back().x, base});
                area.push_back({pts.front().x, base});
                g.fill_poly_v(area, field.y, field.bottom(), series_[s].color, series_[s].color, 0.30f, 0.03f);
            }
            g.polyline(pts, series_[s].color, 1.8f);
        }
        g.pop_rounded_clip();
        // podklad, zeby linia wykresu nie przecinala napisu
        g.fill_rrect({field.x + 5, field.y + 3, g.measure(lane.title, Font::Caps).width + 12, 18}, 5, 0x171d27, 0.82f);
        g.text({field.x + 10, field.y + 5, 200, 15}, lane.title, Font::Caps, MUTED);
        y += h + gap;
    }

    if (!live_) {
        float w = g.measure(OFFLINE_LIVE, Font::Small).width + 28;
        Rect note{r.x + (r.w - w) / 2, top + total_h / 2 - 15, w, 30};
        g.fill_rrect(note, 15, PANEL_HI, 0.92f);
        g.stroke_rrect(note, 15, BORDER);
        g.text(note, OFFLINE_LIVE, Font::Small, TEXT_DIM, Align::Center);
    }
}

void TraceChart::mouse_move(float x, float y) {
    int i = legend_at(x, y);
    if (i != hover_) {
        hover_ = i;
        invalidate();
    }
}

void TraceChart::mouse_leave() {
    if (hover_ != -1) {
        hover_ = -1;
        invalidate();
    }
}

void TraceChart::mouse_down(float x, float y) {
    int i = legend_at(x, y);
    if (i >= 0) {
        shown_[static_cast<size_t>(i)] = !shown_[static_cast<size_t>(i)];
        invalidate();
    }
}

// --------------------------------------------------------------- TextView

float TextView::line_height() { return Gfx::get().measure("Ag", Font::MonoSmall).height; }

Size TextView::measure(float avail_w) { return {avail_w, static_cast<float>(rows_) * line_height() + 20}; }

void TextView::clear() {
    lines_.clear();
    offset_ = 0;
    invalidate();
}

void TextView::set_lines(const std::vector<std::string>& lines, RGB color) {
    lines_.clear();
    for (const auto& l : lines) lines_.push_back({l, color, 0});
    invalidate();
}

void TextView::append(const std::string& line, RGB color, int level) {
    lines_.push_back({line, color, level});
    // ograniczenie dziennika do 600 linii
    while (lines_.size() > 600) lines_.pop_front();
    invalidate();
}

void TextView::set_rows(int rows) {
    if (rows_ != rows) {
        rows_ = rows;
        relayout();
    }
}

void TextView::set_filter(int min_level) {
    if (min_level_ != min_level) {
        min_level_ = min_level;
        follow_ = true;
        invalidate();
    }
}

std::string TextView::joined() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += "\n";
        out += lines_[i].text;
    }
    return out;
}

bool TextView::mouse_wheel(int delta) {
    float view = r.h - 20;
    if (content_h_ <= view) return false;
    float max_off = content_h_ - view;
    follow_ = false;
    offset_ = clampf(offset_ - static_cast<float>(delta) / 120.0f * 3 * line_height(), 0, max_off);
    if (offset_ >= max_off - 1) follow_ = true;
    invalidate();
    return true;
}

void TextView::paint(Gfx& g) {
    g.fill_rrect(r, 10, 0x0f131a);
    g.stroke_rrect(r, 10, BORDER);
    Rect in{r.x + 12, r.y + 10, r.w - 24 - 8, r.h - 20};
    float total = 0;
    float line_h = line_height();
    std::vector<float> heights;
    heights.reserve(lines_.size());
    for (const auto& l : lines_) {
        float h = l.level < min_level_ ? 0.0f : (l.text.empty() ? line_h : g.measure(l.text, Font::MonoSmall, in.w).height);
        heights.push_back(h);
        total += h;
    }
    content_h_ = total;
    float max_off = std::max(0.0f, total - in.h);
    if (follow_) offset_ = max_off;
    offset_ = clampf(offset_, 0, max_off);

    g.push_clip(in);
    float y = in.y - offset_;
    for (size_t i = 0; i < lines_.size(); ++i) {
        bool cut_top = follow_ && y < in.y - 0.5f;
        if (heights[i] > 0 && !cut_top && y + heights[i] >= in.y && y <= in.bottom())
            g.text({in.x, y, in.w, heights[i]}, lines_[i].text, Font::MonoSmall, lines_[i].color, Align::Left, true);
        y += heights[i];
    }
    g.pop_clip();

    if (total > in.h) {
        float bar_h = std::max(20.0f, in.h * in.h / total);
        float bar_y = in.y + (in.h - bar_h) * (max_off > 0 ? offset_ / max_off : 0);
        g.fill_rrect({r.right() - 9, bar_y, 5, bar_h}, 2.5f, BORDER_HI);
    }
}

// --------------------------------------------------------------- ModeTile

ModeTile::ModeTile(const char* icon, std::string title, std::string desc, std::function<void()> command)
    : icon_(icon), title_(std::move(title)), desc_(std::move(desc)), command_(std::move(command)) {}

void ModeTile::set_selected(bool selected) {
    if (selected_ != selected) {
        selected_ = selected;
        invalidate();
    }
}

Size ModeTile::measure(float avail_w) { return {avail_w, 84}; }

void ModeTile::paint(Gfx& g) {
    sel_anim_.set(selected_ ? 1.0f : 0.0f);
    hover_anim_.set(hover_ ? 1.0f : 0.0f);
    animate(sel_anim_, 18);
    animate(hover_anim_, 22);
    float s = sel_anim_.value, h = hover_anim_.value;
    RGB fill = mix(mix(PANEL_HI, ELEV, h * 0.6), ACCENT_DEEP, s * 0.8);
    if (s > 0.01f) g.shadow(r, 12, 12, ACCENT, 0.16f * s);
    g.fill_rrect(r, 12, fill);
    g.stroke_rrect(r, 12, mix(mix(BORDER, BORDER_HI, h), ACCENT, s));
    g.text({r.x + 12, r.y + 12, 22, 22}, icon_, Font::Icon, mix(TEXT_DIM, ACCENT, s), Align::Center);
    if (s > 0.5f) g.text({r.right() - 28, r.y + 10, 18, 20}, ic::check, Font::IconSmall, ACCENT, Align::Center);
    g.text_fit({r.x + 12, r.y + 38, r.w - 20, 18}, title_, Font::BodyStrong, TEXT);
    g.text_fit({r.x + 12, r.y + 57, r.w - 20, 18}, desc_, Font::Tiny, mix(MUTED, TEXT_DIM, s));
}

void ModeTile::mouse_up(float x, float y) {
    if (r.contains(x, y) && command_) command_();
}

// ------------------------------------------------------------- StatusLine

StatusLine::StatusLine() = default;

void StatusLine::set(Level level, const std::string& title, const std::string& detail) {
    if (level != level_) {
        level_ = level;
        invalidate();
    }
    if (title != title_ || detail != detail_) {
        title_ = title;
        detail_ = detail;
        relayout();
    }
}

void StatusLine::set_action(const std::string& text, std::function<void()> command) {
    if (text.empty()) {
        if (action_) {
            action_.reset();
            relayout();
        }
        return;
    }
    if (!action_) {
        action_ = std::make_unique<Button>(text, Button::Small, std::move(command));
        relayout();
    } else {
        action_->set(text);
        action_->set_command(std::move(command));
    }
}

float StatusLine::action_w() { return action_ ? action_->measure(0).w + 10 : 0.0f; }

Size StatusLine::measure(float avail_w) {
    Gfx& g = Gfx::get();
    float tw = std::max(80.0f, avail_w - 36 - action_w());
    float h = g.measure(title_, Font::BodyStrong, tw).height;
    if (!detail_.empty()) h += g.measure(detail_, Font::Small, tw).height + 1;
    return {avail_w, std::max(h, action_ ? 30.0f : 24.0f) + 14};
}

void StatusLine::arrange(const Rect& rc) {
    r = rc;
    if (action_) {
        float w = action_->measure(0).w;
        action_->arrange(snapped({rc.right() - w, rc.y + (rc.h - 30) / 2, w, 30}));
    }
}

void StatusLine::paint(Gfx& g) {
    float cx = r.x + 13, cy = r.y + r.h / 2;
    RGB color = MUTED;
    const char* glyph = nullptr;
    switch (level_) {
    case Level::Ok: color = GOOD; glyph = ic::check; break;
    case Level::Warn: color = WARN; glyph = ic::warn; break;
    case Level::Bad: color = BAD; glyph = ic::cancel; break;
    case Level::Off: color = MUTED; glyph = ic::remove; break;
    case Level::Busy: color = WARN; break;
    }
    g.fill_circle(cx, cy, 13, color, 0.15f);
    if (level_ == Level::Busy) {
        g.spinner(cx, cy, 7, color, Ui::get().time);
        invalidate();
    } else {
        g.text({cx - 11, cy - 11, 22, 22}, glyph, Font::IconSmall, color, Align::Center);
    }
    float tw = std::max(80.0f, r.w - 36 - action_w());
    float title_h = g.measure(title_, Font::BodyStrong, tw).height;
    float detail_h = detail_.empty() ? 0.0f : g.measure(detail_, Font::Small, tw).height + 1;
    float y = r.y + (r.h - title_h - detail_h) / 2;
    g.text({r.x + 36, y, tw, title_h}, title_, Font::BodyStrong, TEXT, Align::Left, true);
    if (!detail_.empty()) g.text({r.x + 36, y + title_h + 1, tw, detail_h}, detail_, Font::Small, MUTED, Align::Left, true);
    if (action_) action_->paint(g);
}

Widget* StatusLine::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    return action_ ? action_->hit_test(x, y) : nullptr;
}

// ---------------------------------------------------------------- Sidebar

namespace {
constexpr float NAV_TOP = 88, NAV_ITEM = 42, NAV_GAP = 2, NAV_HEADER = 30;
}

Sidebar::Sidebar(std::vector<Item> items, std::function<void(int)> command)
    : items_(std::move(items)), command_(std::move(command)) {
    rects_.resize(items_.size());
    badges_.resize(items_.size());
    hover_anim_.resize(items_.size());
}

void Sidebar::select(int index) {
    if (selected_ != index) {
        selected_ = index;
        invalidate();
    }
}

void Sidebar::set_badge(int index, const std::string& text, RGB color) {
    auto& b = badges_[static_cast<size_t>(index)];
    if (b.first != text || b.second != color) {
        b = {text, color};
        invalidate();
    }
}

void Sidebar::set_compact(bool compact) {
    if (compact_ != compact) {
        compact_ = compact;
        indicator_placed_ = false;
        relayout();
    }
}

void Sidebar::set_cluster(bool live, double deg, double range, double throttle, double brake, double clutch) {
    if (live != live_ || std::abs(deg - deg_) >= 0.5 || range != range_ || std::abs(throttle - throttle_) > 0.002 ||
        std::abs(brake - brake_) > 0.002 || std::abs(clutch - clutch_) > 0.002) {
        live_ = live;
        deg_ = deg;
        range_ = range;
        if (range_placed_) range_shown_.set(static_cast<float>(range));
        else range_shown_.snap(static_cast<float>(range));
        range_placed_ = true;
        throttle_ = throttle;
        brake_ = brake;
        clutch_ = clutch;
        invalidate();
    }
}

bool Sidebar::is_drag_area(float x, float y) const { return x < r.right() && y < NAV_TOP - 8; }

void Sidebar::arrange(const Rect& rc) {
    r = rc;
    headers_.clear();
    float y = rc.y + NAV_TOP;
    float pad = compact_ ? 12.0f : 12.0f;
    std::string group;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].group != group) {
            group = items_[i].group;
            if (!compact_) {
                headers_.push_back({y, upper_pl(group)});
                y += NAV_HEADER;
            } else if (i) {
                y += 12;
            }
        }
        rects_[i] = {rc.x + pad, y, rc.w - 2 * pad, NAV_ITEM};
        y += NAV_ITEM + NAV_GAP;
    }
    float cluster_h = compact_ ? 118.0f : 112.0f;
    cluster_ = {rc.x + 12, rc.bottom() - 14 - cluster_h, rc.w - 24, cluster_h};
    show_cluster_ = cluster_.y > y + 6;
}

void Sidebar::paint(Gfx& g) {
    g.fill_rect(r, SIDEBAR);
    g.fill_rect({r.right() - 1, r.y, 1, r.h}, BORDER);

    // znak: logo programu (ta sama ikona co plik .exe, src/res/kierownica.ico)
    constexpr float LOGO = 34.0f;
    float lx = r.x + (compact_ ? r.w / 2 : 36), ly = r.y + 44;
    g.icon(IDI_APP, {lx - LOGO / 2, ly - LOGO / 2, LOGO, LOGO});
    if (!compact_) g.text({lx + 26, ly - 15, 150, 30}, "Kierownica", Font::Head, TEXT);

    for (const auto& [y, title] : headers_) g.text({r.x + 24, y + 8, r.w - 40, 16}, title, Font::Caps, MUTED);

    // znacznik wybranej pozycji jedzie miedzy wierszami
    if (!indicator_placed_) {
        indicator_.snap(rects_[static_cast<size_t>(selected_)].y);
        indicator_placed_ = true;
    }
    indicator_.set(rects_[static_cast<size_t>(selected_)].y);
    animate(indicator_, 20);
    Rect sel = rects_[static_cast<size_t>(selected_)];
    sel.y = indicator_.value;
    g.fill_rrect(sel, 10, PANEL_HI);
    g.stroke_rrect(sel, 10, BORDER);
    g.fill_rrect({sel.x + 4, sel.y + 11, 3, sel.h - 22}, 1.5f, ACCENT);

    for (size_t i = 0; i < items_.size(); ++i) {
        bool is_sel = static_cast<int>(i) == selected_;
        hover_anim_[i].set(static_cast<int>(i) == hover_ && !is_sel ? 1.0f : 0.0f);
        animate(hover_anim_[i], 22);
        float h = hover_anim_[i].value;
        const Rect& rc = rects_[i];
        if (h > 0.01f) g.fill_rrect(rc, 10, PANEL_HI, h * 0.7f);
        RGB fg = is_sel ? TEXT : mix(TEXT_DIM, TEXT, h);
        RGB icon = is_sel ? ACCENT : fg;
        if (compact_) {
            g.text(rc, items_[i].icon, Font::Icon, icon, Align::Center);
        } else {
            g.text({rc.x + 14, rc.y, 24, rc.h}, items_[i].icon, Font::Icon, icon, Align::Center);
            g.text({rc.x + 46, rc.y, rc.w - 90, rc.h}, items_[i].label, is_sel ? Font::BodyStrong : Font::Body, fg);
        }
        const auto& [text, color] = badges_[i];
        if (!text.empty()) {
            if (compact_) {
                g.fill_circle(rc.right() - 12, rc.y + 11, 4, color);
            } else {
                float w = std::max(20.0f, g.measure(text, Font::TinyStrong).width + 12);
                Rect b{rc.right() - 10 - w, rc.y + (rc.h - 20) / 2, w, 20};
                g.fill_rrect(b, 10, color, 0.18f);
                g.text(b, text, Font::TinyStrong, color, Align::Center);
            }
        }
    }
    if (show_cluster_) paint_cluster(g);
}

void Sidebar::paint_cluster(Gfx& g) {
    const Rect& c = cluster_;
    g.fill_rrect_v(c, 12, 0x161b25, 0x11151c);
    g.stroke_rrect(c, 12, BORDER);
    std::string angle = !live_ ? "—" : (std::abs(deg_) < 0.5 ? "0°" : std::format("{:+.0f}°", deg_));
    const std::pair<double, RGB> pedals[3] = {{throttle_, GOOD}, {brake_, BAD}, {clutch_, WARN}};
    if (compact_) {
        g.text({c.x, c.y + 8, c.w, 22}, angle, Font::Num, TEXT, Align::Center);
        float bx = c.x + (c.w - 8 * 3 - 6 * 2) / 2;
        for (int i = 0; i < 3; ++i) {
            Rect bar{bx + static_cast<float>(i) * 14, c.y + 36, 8, 70};
            g.fill_rrect(bar, 4, TRACK);
            float h = std::round(bar.h * static_cast<float>(pedals[i].first));
            if (live_ && h >= 2) {
                g.push_rounded_clip(bar, 4);
                g.fill_rect({bar.x, bar.bottom() - h, bar.w, h}, pedals[i].second);
                g.pop_rounded_clip();
            }
        }
        return;
    }
    g.text({c.x + 14, c.y + 8, c.w - 28, 30}, angle, Font::NumLarge, TEXT);
    bool moving = range_placed_ && range_shown_.step(Ui::get().dt);
    if (moving) invalidate();
    g.text({c.x + 14, c.y + 8, c.w - 28, 30}, std::format("z {:.0f}°", range_placed_ ? range_shown_.value : static_cast<float>(range_)), Font::Tiny,
           moving ? ACCENT_HI : MUTED, Align::Right);
    const char* names[3] = {"G", "H", "S"};
    float y = c.y + 46;
    for (int i = 0; i < 3; ++i) {
        g.text({c.x + 14, y, 12, 16}, names[i], Font::TinyStrong, MUTED);
        Rect bar{c.x + 30, y + 5, c.w - 44, 6};
        g.fill_rrect(bar, 3, TRACK);
        float w = std::round(bar.w * static_cast<float>(pedals[i].first));
        if (live_ && w >= 2) g.fill_rrect({bar.x, bar.y, w, bar.h}, 3, pedals[i].second);
        y += 19;
    }
}

int Sidebar::item_at(float x, float y) const {
    for (size_t i = 0; i < rects_.size(); ++i)
        if (rects_[i].contains(x, y)) return static_cast<int>(i);
    return -1;
}

void Sidebar::mouse_move(float x, float y) {
    int i = item_at(x, y);
    if (i != hover_) {
        hover_ = i;
        invalidate();
    }
}

void Sidebar::mouse_leave() {
    hover_ = -1;
    invalidate();
}

void Sidebar::mouse_down(float x, float y) {
    int i = item_at(x, y);
    if (i >= 0 && command_) command_(i);
}

const std::string* Sidebar::tooltip() const {
    return (compact_ && hover_ >= 0) ? &items_[static_cast<size_t>(hover_)].label : nullptr;
}

// ----------------------------------------------------------------- TopBar

namespace {
constexpr float CAPTION_W = 46, CAPTION_H = 34;
}

TopBar::TopBar(std::function<void(int)> on_caption) : actions_(std::make_unique<Box>(true)), on_caption_(std::move(on_caption)) {
    actions = actions_.get();
}

void TopBar::set_title(const std::string& title, const std::string& subtitle) {
    if (title != title_ || subtitle != subtitle_) {
        title_ = title;
        subtitle_ = subtitle;
        invalidate();
    }
}

void TopBar::set_maximized(bool maximized) {
    if (maximized_ != maximized) {
        maximized_ = maximized;
        invalidate();
    }
}

Rect TopBar::caption_rect(int caption) const { return {r.right() - static_cast<float>(4 - caption) * CAPTION_W, r.y, CAPTION_W, CAPTION_H}; }

int TopBar::caption_at(float x, float y) const {
    for (int c = 1; c <= 3; ++c)
        if (caption_rect(c).contains(x, y)) return c;
    return 0;
}

void TopBar::set_caption_hover(int caption) {
    if (hover_ != caption) {
        hover_ = caption;
        invalidate();
    }
}

void TopBar::press_caption(int caption) {
    if (pressed_ != caption) {
        pressed_ = caption;
        invalidate();
    }
}

void TopBar::arrange(const Rect& rc) {
    r = rc;
    float w = actions_->measure(rc.w).w;
    float h = actions_->measure(w).h;
    actions_->arrange(snapped({rc.right() - 3 * CAPTION_W - 14 - w, rc.y + (rc.h - h) / 2 + 3, w, h}));
}

void TopBar::paint(Gfx& g) {
    float left = r.x + 26;
    float right_edge = actions_->r.x - 16;
    float tw = std::max(60.0f, right_edge - left);
    g.text({left, r.y + 13, tw, 34}, title_, Font::Title, TEXT);
    g.text({left, r.y + 46, tw, 18}, subtitle_, Font::Small, MUTED);
    actions_->paint(g);

    const char* glyphs[3] = {ic::min_window, maximized_ ? ic::restore_window : ic::max_window, ic::close_window};
    for (int c = 1; c <= 3; ++c) {
        Rect rc = caption_rect(c);
        bool hot = hover_ == c;
        if (hot) g.fill_rect(rc, c == 3 ? 0xc42b1c : ELEV, (pressed_ == c) ? 0.8f : 1.0f);
        g.text(rc, glyphs[c - 1], Font::IconSmall, hot ? WHITE : TEXT_DIM, Align::Center);
    }
}

Widget* TopBar::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    if (caption_at(x, y)) return this;
    return actions_->hit_test(x, y);
}

void TopBar::mouse_move(float x, float y) { set_caption_hover(caption_at(x, y)); }
void TopBar::mouse_leave() { set_caption_hover(0); }
void TopBar::mouse_down(float x, float y) { press_caption(caption_at(x, y)); }

void TopBar::mouse_up(float x, float y) {
    int c = caption_at(x, y);
    bool click = c != 0 && c == pressed_;
    pressed_ = 0;
    invalidate();
    if (click && on_caption_) on_caption_(c);
}

// ---------------------------------------------------------------- Popover

Popover::Popover(WidgetPtr content, const Rect& anchor, float width) : content_(std::move(content)), anchor_(anchor), width_(width) {
    show_.set(1.0f);
}

void Popover::arrange(const Rect& rc) {
    r = rc;
    float h = content_->measure(width_ - 32).h + 32;
    float x = clampf(anchor_.right() - width_, 8, rc.w - width_ - 8);
    float y = anchor_.bottom() + 6;
    if (y + h > rc.h - 8) y = std::max(8.0f, rc.h - 8 - h);
    box_ = snapped({x, y, width_, h});
    content_->arrange(snapped({box_.x + 16, box_.y + 16, width_ - 32, h - 32}));
}

void Popover::paint(Gfx& g) {
    animate(show_, 24);
    float a = show_.value;
    g.begin_opacity(a);
    g.push_translate(0, (1 - a) * -6);
    g.shadow(box_, 14, 18, BLACK, 0.65f, 8);
    g.fill_rrect(box_, 14, 0x1a202a);
    g.stroke_rrect(box_, 14, BORDER_HI);
    content_->paint(g);
    g.pop_translate();
    g.end_opacity();
}

Widget* Popover::hit_test(float x, float y) {
    if (box_.contains(x, y))
        if (Widget* hit = content_->hit_test(x, y)) return hit;
    return this;
}

void Popover::mouse_down(float x, float y) {
    if (!box_.contains(x, y)) Ui::get().close_overlay();
}

void Popover::mouse_rdown(float x, float y) { mouse_down(x, y); }

bool Popover::key_down(unsigned vk) {
    if (vk == VK_ESCAPE) {
        Ui::get().close_overlay();
        return true;
    }
    return false;
}

// -------------------------------------------------------------- PopupMenu

class MenuOverlay final : public Widget {
public:
    MenuOverlay(std::string title, std::vector<PopupMenu::Item> items, float x, float y)
        : title_(std::move(title)), items_(std::move(items)) {
        Gfx& g = Gfx::get();
        float width = 200;
        for (const auto& it : items_) {
            if (it.separator) continue;
            float w = 34 + g.measure(it.label, Font::SmallStrong).width + 24;
            if (!it.note.empty()) w += g.measure(it.note, Font::Tiny).width + 14;
            width = std::max(width, w);
        }
        if (!title_.empty()) width = std::max(width, g.measure(title_, Font::Caps).width + 34);
        float height = 12 + (title_.empty() ? 0.0f : TITLE_H);
        for (const auto& it : items_) height += it.separator ? 9.0f : ROW;
        Size win = Ui::get().window;
        // menu zostaje na ekranie, takze przy kliknieciu przy krawedzi
        box_ = {std::max(4.0f, std::min(x, win.w - width - 6)), std::max(4.0f, std::min(y, win.h - height - 6)), width, height};
        r = {0, 0, win.w, win.h};
        show_.set(1.0f);
    }

    Size measure(float) override { return Ui::get().window; }
    Widget* hit_test(float, float) override { return this; }
    bool hand_cursor() const override { return hover_ >= 0; }

    void paint(Gfx& g) override {
        animate(show_, 28);
        g.begin_opacity(show_.value);
        g.shadow(box_, 12, 16, BLACK, 0.65f, 7);
        g.fill_rrect(box_, 12, 0x1f2631);
        g.stroke_rrect(box_, 12, BORDER_HI);
        Rect in = box_.inset(6);
        float y = in.y;
        if (!title_.empty()) {
            g.text({in.x + 10, y, in.w - 20, TITLE_H}, title_, Font::Caps, MUTED);
            y += TITLE_H;
        }
        for (size_t i = 0; i < items_.size(); ++i) {
            const auto& it = items_[i];
            if (it.separator) {
                g.fill_rect({in.x + 6, y + 4, in.w - 12, 1}, BORDER_HI, 0.7f);
                y += 9;
                continue;
            }
            Rect row{in.x, y, in.w, ROW};
            if (static_cast<int>(i) == hover_) g.fill_rrect(row, 7, ELEV);
            if (it.checked) g.text({row.x + 6, y, 20, ROW}, ic::check, Font::IconSmall, ACCENT, Align::Center);
            g.text({row.x + 32, y, row.w - 44, ROW}, it.label, it.checked ? Font::SmallStrong : Font::Small, TEXT);
            if (!it.note.empty()) g.text({row.x, y, row.w - 10, ROW}, it.note, Font::Tiny, WARN, Align::Right);
            y += ROW;
        }
        g.end_opacity();
    }

    int item_at(float px, float py) const {
        Rect in = box_.inset(6);
        if (!in.contains(px, py)) return -1;
        float y = in.y + (title_.empty() ? 0.0f : TITLE_H);
        for (size_t i = 0; i < items_.size(); ++i) {
            float h = items_[i].separator ? 9.0f : ROW;
            if (py >= y && py < y + h) return items_[i].separator ? -1 : static_cast<int>(i);
            y += h;
        }
        return -1;
    }

    void mouse_move(float x, float y) override {
        int i = item_at(x, y);
        if (i != hover_) {
            hover_ = i;
            invalidate();
        }
    }

    void mouse_down(float x, float y) override {
        if (!box_.contains(x, y)) Ui::get().close_overlay();
    }
    void mouse_rdown(float x, float y) override { mouse_down(x, y); }

    void mouse_up(float x, float y) override {
        int i = item_at(x, y);
        if (i < 0) return;
        auto command = items_[static_cast<size_t>(i)].command;
        Ui::get().close_overlay();
        if (command) command();
    }

    bool key_down(unsigned vk) override {
        if (vk == VK_ESCAPE) {
            Ui::get().close_overlay();
            return true;
        }
        return false;
    }

private:
    static constexpr float ROW = 30;
    static constexpr float TITLE_H = 28;
    std::string title_;
    std::vector<PopupMenu::Item> items_;
    Rect box_;
    int hover_ = -1;
    Anim show_;
};

void PopupMenu::add(std::string label, std::function<void()> command, bool checked, std::string note) {
    Item it;
    it.label = std::move(label);
    it.command = std::move(command);
    it.checked = checked;
    it.note = std::move(note);
    items_.push_back(std::move(it));
}

void PopupMenu::add_separator() {
    Item it;
    it.separator = true;
    items_.push_back(std::move(it));
}

void PopupMenu::popup(float x, float y) {
    Ui::get().open_overlay(std::make_unique<MenuOverlay>(title_, items_, x, y));
}

} // namespace wb::ui

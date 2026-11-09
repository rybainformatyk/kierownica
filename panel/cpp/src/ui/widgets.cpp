#include "widgets.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

#include "../util.h"

namespace wb::ui {

namespace {

constexpr float PI = std::numbers::pi_v<float>;

float clampf(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

} // namespace

void Widget::set_visible(bool v) {
    if (visible != v) {
        visible = v;
        relayout();
    }
}

Ui& Ui::get() {
    static Ui instance;
    return instance;
}

void Ui::open_overlay(WidgetPtr overlay) {
    close_overlay();
    overlay_ = std::move(overlay);
    invalidate();
}

void Ui::close_overlay() {
    if (overlay_) trash_.push_back(std::move(overlay_));
    invalidate();
}

void Ui::set_focus(Widget* w) {
    if (focus_ == w) return;
    Widget* old = focus_;
    focus_ = w;
    if (old) old->focus_changed(false);
    if (w) w->focus_changed(true);
    invalidate();
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
            it.w->arrange({x, y, w, h});
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
            it.w->arrange({x, y, w, s.h});
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
        cell.w->arrange({x, y, w, h});
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

// ------------------------------------------------------------------- Card

std::string upper_pl(const std::string& s) {
    std::wstring w = widen(s);
    CharUpperBuffW(w.data(), static_cast<DWORD>(w.size()));
    return narrow(w);
}

Card::Card(std::string title, std::string subtitle, float wrap)
    : title_(upper_pl(title)), body_(std::make_unique<Box>(false)), right_(std::make_unique<Box>(true)) {
    body = body_.get();
    right_slot = right_.get();
    if (!subtitle.empty())
        body_->add(std::make_unique<Label>(std::move(subtitle), Font::Small, MUTED, wrap), Pack{.bottom = 8});
}

float Card::header_h(float inner_w) {
    float title = Gfx::get().measure(title_, Font::SmallBold).height;
    return std::max(title, right_->measure(inner_w).h);
}

Size Card::measure(float avail_w) {
    float inner = std::max(0.0f, avail_w - 32);
    Size b = body_->measure(inner);
    return {avail_w, 1 + 12 + header_h(inner) + 10 + 1 + 11 + b.h + 14 + 1};
}

void Card::arrange(const Rect& rc) {
    r = rc;
    float inner = std::max(0.0f, rc.w - 32);
    float h = header_h(inner);
    float rw = right_->measure(inner).w;
    right_->arrange({rc.right() - 16 - rw, rc.y + 13, rw, h});
    body_->arrange({rc.x + 16, rc.y + 13 + h + 10 + 1 + 11, inner, std::max(0.0f, rc.bottom() - 15 - (rc.y + 13 + h + 22))});
}

void Card::paint(Gfx& g) {
    g.fill_rrect(r, 10, PANEL);
    g.stroke_rrect(r, 10, BORDER);
    float inner = std::max(0.0f, r.w - 32);
    float h = header_h(inner);
    g.fill_rect({r.x + 16, r.y + 13 + (h - 13) / 2, 3, 13}, ACCENT);
    g.text({r.x + 28, r.y + 13, inner - 12, h}, title_, Font::SmallBold, TEXT_DIM);
    g.fill_rect({r.x + 16, r.y + 13 + h + 10, inner, 1}, BORDER);
    right_->paint(g);
    body_->paint(g);
}

Widget* Card::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    if (Widget* hit = right_->hit_test(x, y)) return hit;
    return body_->hit_test(x, y);
}

Strip::Strip(std::string title) : title_(std::move(title)), row_(std::make_unique<Box>(true)) { row = row_.get(); }

Size Strip::measure(float avail_w) {
    float inner = std::max(0.0f, avail_w - 32 - 120);
    return {avail_w, std::max(row_->measure(inner).h, 18.0f) + 20};
}

void Strip::arrange(const Rect& rc) {
    r = rc;
    float title = Gfx::get().measure(title_, Font::SmallBold).width + 12;
    row_->arrange({rc.x + 16 + title + 8, rc.y + 10, std::max(0.0f, rc.w - 32 - title - 8), rc.h - 20});
}

void Strip::paint(Gfx& g) {
    g.fill_rrect(r, 10, PANEL);
    g.stroke_rrect(r, 10, BORDER);
    g.fill_rect({r.x + 16, r.y + (r.h - 13) / 2, 3, 13}, ACCENT);
    g.text({r.x + 28, r.y, 240, r.h}, title_, Font::SmallBold, TEXT_DIM);
    row_->paint(g);
}

Widget* Strip::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    return row_->hit_test(x, y);
}

// ----------------------------------------------------------------- Scroll

void Scroll::arrange(const Rect& rc) {
    r = rc;
    float w = std::max(1.0f, rc.w - GUTTER);
    content_h_ = content_->measure(w).h;
    offset_ = clampf(offset_, 0, std::max(0.0f, content_h_ - rc.h));
    content_->arrange({rc.x, rc.y - offset_, w, content_h_});
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
    float h = std::max(24.0f, r.h * r.h / content_h_);
    float span = r.h - h;
    float max_off = content_h_ - r.h;
    float y = r.y + (max_off > 0 ? span * offset_ / max_off : 0);
    return {r.right() - BAR_W - 1, y, BAR_W, h};
}

void Scroll::paint(Gfx& g) {
    g.push_clip(r);
    content_->paint(g);
    g.pop_clip();
    if (content_h_ > r.h) {
        Rect t = thumb();
        RGB color = dragging_ ? ACCENT_DIM : ELEV;
        g.fill_rrect(t, 4, color);
    }
}

Widget* Scroll::hit_test(float x, float y) {
    if (!visible || !r.contains(x, y)) return nullptr;
    if (content_h_ > r.h && x >= r.right() - BAR_W - 2) return this;
    if (Widget* hit = content_->hit_test(x, y)) return hit;
    return this;
}

void Scroll::mouse_down(float x, float y) {
    if (content_h_ <= r.h || x < r.right() - BAR_W - 2) return;
    Rect t = thumb();
    if (!t.contains(x, y)) {
        // klik w rowek przenosi uchwyt pod kursor
        float span = r.h - t.h;
        if (span > 0) set_offset((y - r.y - t.h / 2) / span * (content_h_ - r.h));
    }
    dragging_ = true;
    drag_y_ = y;
    drag_offset_ = offset_;
    invalidate();
}

void Scroll::mouse_move(float, float y) {
    if (!dragging_) return;
    float span = r.h - thumb().h;
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
    relayout();
    if (on_changed) on_changed(index);
}

void Pages::arrange(const Rect& rc) {
    r = rc;
    pages_[static_cast<size_t>(current_)]->arrange(rc);
}

void Pages::paint(Gfx& g) { pages_[static_cast<size_t>(current_)]->paint(g); }

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
    Font font = style_ == Accent ? Font::Bold : (compact ? Font::Small : Font::Base);
    auto s = Gfx::get().measure(text_, font);
    float px = compact ? 11.0f : 14.0f, py = compact ? 5.0f : 8.0f;
    return {width_ > 0 ? width_ : s.width + 2 * px + 2, s.height + 2 * py + 2};
}

void Button::paint(Gfx& g) {
    RGB fill = PANEL_HI, edge = BORDER, fg = TEXT;
    Font font = Font::Base;
    switch (style_) {
    case Normal:
    case Small:
        font = style_ == Small ? Font::Small : Font::Base;
        if (!enabled_) { fill = PANEL; edge = BORDER; fg = MUTED; }
        else if (pressed_ && hover_) { fill = ACCENT_DEEP; edge = ACCENT_DIM; }
        else if (hover_) { fill = ELEV; edge = BORDER_HI; }
        break;
    case Accent:
        font = Font::Bold;
        fg = ACCENT_INK;
        fill = edge = ACCENT;
        if (!enabled_) { fill = edge = ACCENT_DIM; fg = 0x4d6678; }
        else if (pressed_ && hover_) fill = edge = 0x2f9fd8;
        else if (hover_) fill = edge = 0x6ccfff;
        break;
    case Ghost:
        font = Font::Small;
        fill = PANEL; fg = TEXT_DIM;
        if (hover_ || pressed_) { fill = PANEL_HI; edge = BORDER_HI; fg = TEXT; }
        break;
    }
    g.fill_rrect(r, 7, edge);
    g.fill_rrect({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 6, fill);
    g.text(r, text_, font, fg, Align::Center);
}

void Button::mouse_up(float x, float y) {
    bool click = pressed_ && r.contains(x, y) && enabled_;
    pressed_ = false;
    invalidate();
    if (click && command_) command_();
}

// --------------------------------------------------------------- Checkbox

Checkbox::Checkbox(std::string text, bool value, std::function<void(bool)> command, Font font)
    : text_(std::move(text)), value_(value), command_(std::move(command)), font_(font) {}

void Checkbox::set(bool v) {
    if (value_ != v) {
        value_ = v;
        invalidate();
    }
}

Size Checkbox::measure(float) {
    auto s = Gfx::get().measure(text_, font_);
    return {16 + 8 + s.width + 6, std::max(16.0f, s.height) + 8};
}

void Checkbox::paint(Gfx& g) {
    Rect box{r.x + 2, r.y + (r.h - 16) / 2, 16, 16};
    RGB fill = PANEL_HI, edge = BORDER;
    if (value_) fill = edge = hover_ ? 0x6ccfff : ACCENT;
    else if (hover_) { fill = ELEV; edge = BORDER_HI; }
    g.fill_rrect(box, 4.5f, edge);
    g.fill_rrect({box.x + 1, box.y + 1, 14, 14}, 3.5f, fill);
    if (value_) g.polyline({{box.x + 4.2f, box.y + 8.3f}, {box.x + 6.9f, box.y + 11.0f}, {box.x + 11.9f, box.y + 5.3f}}, ACCENT_INK, 2.0f);
    g.text({r.x + 26, r.y, r.w - 26, r.h}, text_, font_, (hover_ || value_) ? TEXT : TEXT_DIM);
}

void Checkbox::mouse_up(float x, float y) {
    if (!r.contains(x, y)) return;
    value_ = !value_;
    invalidate();
    if (command_) command_(value_);
}

// ------------------------------------------------------------------ Combo

namespace {

// Rozwinieta lista - nakladka na okno, klik obok ja zamyka.
class ComboList final : public Widget {
public:
    static constexpr float ROW = 24;
    static constexpr int MAX_ROWS = 14;

    ComboList(Rect anchor, std::vector<std::string> items, int index, std::function<void(int)> pick)
        : items_(std::move(items)), selected_(index), pick_(std::move(pick)) {
        int shown = std::min(static_cast<int>(items_.size()), MAX_ROWS);
        float h = static_cast<float>(shown) * ROW + 2;
        Size win = Ui::get().window;
        float y = anchor.bottom() + 2;
        if (y + h > win.h - 4) y = std::max(4.0f, anchor.y - h - 2);
        r_list_ = {anchor.x, y, anchor.w, h};
        r = {0, 0, win.w, win.h};
        // od razu widac biezacy wybor
        first_ = std::clamp(index - shown / 2, 0, std::max(0, static_cast<int>(items_.size()) - shown));
        hover_ = index;
    }

    Size measure(float) override { return Ui::get().window; }
    Widget* hit_test(float, float) override { return this; }

    void paint(Gfx& g) override {
        g.fill_rrect(r_list_, 6, BORDER_HI);
        Rect in{r_list_.x + 1, r_list_.y + 1, r_list_.w - 2, r_list_.h - 2};
        g.fill_rrect(in, 5, PANEL_HI);
        g.push_clip(in);
        int shown = static_cast<int>(in.h / ROW);
        for (int i = 0; i < shown && first_ + i < static_cast<int>(items_.size()); ++i) {
            int idx = first_ + i;
            Rect row{in.x, in.y + static_cast<float>(i) * ROW, in.w, ROW};
            bool sel = idx == hover_;
            if (sel) g.fill_rect(row, ACCENT);
            g.text({row.x + 8, row.y, row.w - 16, row.h}, items_[static_cast<size_t>(idx)], Font::Base, sel ? ACCENT_INK : TEXT);
        }
        g.pop_clip();
        if (static_cast<int>(items_.size()) > shown) {
            float frac = static_cast<float>(shown) / static_cast<float>(items_.size());
            float top = static_cast<float>(first_) / static_cast<float>(items_.size());
            g.fill_rrect({in.right() - 5, in.y + top * in.h, 4, std::max(16.0f, frac * in.h)}, 2, ELEV);
        }
    }

    void mouse_move(float x, float y) override {
        int i = row_at(x, y);
        if (i != hover_ && i >= 0) {
            hover_ = i;
            invalidate();
        }
    }

    void mouse_down(float x, float y) override {
        if (!r_list_.contains(x, y)) {
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
        if (vk == VK_ESCAPE) {
            Ui::get().close_overlay();
            return true;
        }
        return false;
    }

    bool hand_cursor() const override { return true; }

private:
    int row_at(float x, float y) const {
        if (!r_list_.contains(x, y)) return -1;
        int i = first_ + static_cast<int>((y - r_list_.y - 1) / ROW);
        return i < static_cast<int>(items_.size()) ? i : -1;
    }

    Rect r_list_;
    std::vector<std::string> items_;
    int selected_;
    int hover_ = -1;
    int first_ = 0;
    bool pressed_inside_ = false;
    std::function<void(int)> pick_;
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
    float w = static_cast<float>(width_chars_) * 7.2f + 16 + 22;
    return {std::min(w, std::max(avail_w, 60.0f)), Gfx::get().measure("Ag", Font::Base).height + 14};
}

void Combo::paint(Gfx& g) {
    g.fill_rrect(r, 6, hover_ ? BORDER_HI : BORDER);
    g.fill_rrect({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 5, hover_ ? ELEV : PANEL_HI);
    g.text({r.x + 9, r.y, r.w - 9 - 22, r.h}, text(), Font::Base, TEXT);
    float cx = r.right() - 13, cy = r.y + r.h / 2;
    g.polyline({{cx - 4, cy - 2}, {cx, cy + 2}, {cx + 4, cy - 2}}, hover_ ? TEXT : TEXT_DIM, 1.8f);
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

Size Entry::measure(float) {
    return {static_cast<float>(width_chars_) * 7.2f + 18, Gfx::get().measure("Ag", Font::Base).height + 14};
}

void Entry::paint(Gfx& g) {
    bool focus = Ui::get().focus() == this;
    g.fill_rrect(r, 6, focus ? ACCENT : BORDER);
    g.fill_rrect({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, 5, PANEL_HI);
    g.text({r.x + 9, r.y, r.w - 18, r.h}, text_, Font::Base, TEXT);
    if (focus) {
        caret_ = std::min(caret_, text_.size());
        float x = r.x + 9 + (caret_ ? g.measure(text_.substr(0, caret_), Font::Base).width : 0);
        g.line(x, r.y + 6, x, r.bottom() - 6, TEXT, 1.2f);
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
        Ui::get().set_focus(nullptr);
        break;
    default:
        return false;
    }
    invalidate();
    return true;
}

void Entry::char_input(wchar_t c) {
    if (c < 32 || c > 126) return;
    text_.insert(caret_++, 1, static_cast<char>(c));
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
    float pad = std::max(2.0f, std::floor(size_ / 5));
    float rad = size_ / 2 - pad;
    if (glow_) {
        g.fill_circle(cx, cy, size_ / 2, mix(bg_, color_, 0.22));
        g.fill_circle(cx, cy, rad + 1, mix(bg_, color_, 0.45));
    }
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

Size StatRow::measure(float avail_w) { return {avail_w, Gfx::get().measure("Ag", Font::Small).height + 3}; }

void StatRow::paint(Gfx& g) {
    g.text({r.x, r.y, r.w, r.h}, label_, Font::Small, MUTED);
    g.text({r.x, r.y, r.w, r.h}, value_, Font::MonoBold, color_, Align::Right);
}

// ----------------------------------------------------------------- Slider

Slider::Slider(std::string label, double from, double to, double value, Format fmt, double resolution,
               std::function<void(double)> command, std::string hint)
    : label_(std::move(label)), hint_(std::move(hint)), from_(from), to_(to), value_(value), resolution_(resolution),
      fmt_(std::move(fmt)), command_(std::move(command)) {}

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

Rect Slider::track_rect() const {
    float head = Gfx::get().measure("Ag", Font::SmallBold).height + 4;
    return {r.x, r.y + head + 4, r.w, 22};
}

double Slider::value_at(float x) const {
    float x0 = r.x + 9, x1 = std::max(r.x + 10, r.right() - 9);
    float t = (x - x0) / std::max(1.0f, x1 - x0);
    t = clampf(t, 0, 1);
    return snap(from_ + (to_ - from_) * t);
}

float Slider::x_of(double v) const {
    float x0 = r.x + 9, x1 = std::max(r.x + 10, r.right() - 9);
    double range = to_ - from_;
    double t = std::abs(range) < 1e-9 ? 0.0 : (v - from_) / range;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return x0 + (x1 - x0) * static_cast<float>(t);
}

Size Slider::measure(float avail_w) {
    Gfx& g = Gfx::get();
    float h = g.measure("Ag", Font::SmallBold).height + 4 + 4 + 22;
    if (!hint_.empty() && avail_w > 0) h += 3 + g.measure(hint_, Font::Tiny, avail_w).height;
    return {avail_w, h};
}

void Slider::paint(Gfx& g) {
    float head_h = g.measure("Ag", Font::SmallBold).height + 4;
    g.text({r.x, r.y, r.w, head_h}, label_, Font::Small, TEXT_DIM);
    std::string value = fmt_(value_);
    float vw = g.measure(value, Font::SmallBold).width + 14;
    Rect vr{r.right() - vw, r.y, vw, head_h};
    g.fill_rect(vr, mix(PANEL, ACCENT, 0.14));
    g.text(vr, value, Font::SmallBold, ACCENT, Align::Center);

    Rect t = track_rect();
    float cy = t.y + t.h / 2;
    g.fill_rrect({t.x, cy - 3, t.w, 6}, 3, TRACK);
    float x = x_of(value_);
    if (x > t.x + 3) g.fill_rrect({t.x, cy - 3, x - t.x, 6}, 3, ACCENT);
    if (hover_ || dragging_) g.fill_circle(x, cy, 12, ACCENT, 64.0f / 255.0f);
    g.fill_circle(x, cy, 8, ACCENT);
    g.fill_circle(x, cy, 6, TEXT);

    if (!hint_.empty())
        g.text({r.x, t.bottom() + 3, r.w, r.bottom() - t.bottom() - 3}, hint_, Font::Tiny, MUTED, Align::Left, true);
}

Widget* Slider::hit_test(float x, float y) { return (visible && track_rect().contains(x, y)) ? this : nullptr; }

void Slider::mouse_down(float x, float) {
    dragging_ = true;
    apply(value_at(x));
    invalidate();
}

void Slider::mouse_move(float x, float) {
    if (dragging_) apply(value_at(x));
}

void Slider::mouse_up(float, float) {
    dragging_ = false;
    invalidate();
}

// ------------------------------------------------------------------ paski

void BarMeter::set(double value, std::optional<int> raw) {
    value = value != value ? 0.0 : std::max(0.0, std::min(1.0, value));
    if (std::abs(value - value_) > 0.0005 || raw != raw_) {
        value_ = value;
        raw_ = raw;
        invalidate();
    }
}

Size BarMeter::measure(float avail_w) { return {avail_w, Gfx::get().measure("Ag", Font::Bold).height + 5 + 16}; }

void BarMeter::paint(Gfx& g) {
    float head = g.measure("Ag", Font::Bold).height;
    g.text({r.x, r.y, r.w, head}, label_, Font::Small, TEXT_DIM);
    g.text({r.x, r.y, r.w, head}, std::format("{:.0f} %", value_ * 100), Font::Bold, value_ > 0.002 ? TEXT : MUTED, Align::Right);
    if (raw_) g.text({r.x, r.y, r.w - 58, head}, std::to_string(*raw_), Font::MonoSmall, MUTED, Align::Right);

    Rect bar{r.x, r.y + head + 5, r.w, 16};
    g.fill_rrect(bar, 8, TRACK);
    if (bar.w > 60)
        for (int i = 1; i <= 3; ++i) {
            float x = bar.x + bar.w * static_cast<float>(i) / 4;
            g.line(x, bar.y + 3, x, bar.bottom() - 3, mix(TRACK, BORDER_HI, 0.55));
        }
    float fill = static_cast<float>(value_) * bar.w;
    if (fill >= 1.5f) g.fill_rrect({bar.x, bar.y, fill, bar.h}, 8, color_);
}

void TorqueMeter::set(double value) {
    value = value != value ? 0.0 : std::max(-1.0, std::min(1.0, value));
    if (std::abs(value - value_) >= 0.002) {
        value_ = value;
        invalidate();
    }
}

void TorqueMeter::paint(Gfx& g) {
    g.fill_rrect(r, r.h / 2, TRACK);
    float mid = r.x + r.w / 2;
    float len = static_cast<float>(std::abs(value_)) * r.w / 2;
    if (len >= 2) {
        Rect fill = value_ >= 0 ? Rect{mid, r.y + 2, len, r.h - 4} : Rect{mid - len, r.y + 2, len, r.h - 4};
        g.fill_rrect(fill, std::max(1.0f, r.h / 2 - 2), mix(VIOLET, BAD, std::max(0.0, std::abs(value_) - 0.6) / 0.4));
    }
    for (float f : {0.2f, 0.35f, 0.65f, 0.8f}) {
        float x = r.x + r.w * f;
        g.line(x, r.y + r.h * 0.3f, x, r.y + r.h * 0.7f, BORDER_HI);
    }
    g.line(mid, r.y, mid, r.bottom(), BORDER_HI);
    g.text(r, std::format("{:+.0f} %", value_ * 100), Font::SmallBold, std::abs(value_) > 0.02 ? TEXT : MUTED, Align::Center);
}

void PedalGauge::set(double value, std::optional<int> raw, bool enabled) {
    value = value != value ? 0.0 : (enabled ? std::max(0.0, std::min(1.0, value)) : 0.0);
    if (!enabled) raw.reset();
    if (std::abs(value - value_) > 0.0005 || raw != raw_ || enabled != enabled_) {
        value_ = value;
        raw_ = raw;
        enabled_ = enabled;
        invalidate();
    }
}

Size PedalGauge::measure(float) {
    Gfx& g = Gfx::get();
    return {60, g.measure("Ag", Font::SmallBold).height + 6 + 140 + 4 + g.measure("Ag", Font::Heading).height +
                    g.measure("Ag", Font::MonoSmall).height};
}

void PedalGauge::paint(Gfx& g) {
    float name_h = g.measure("Ag", Font::SmallBold).height;
    g.text({r.x, r.y, r.w, name_h}, label_, Font::SmallBold, enabled_ ? TEXT_DIM : MUTED, Align::Center);
    Rect bar{r.x + (r.w - 44) / 2, r.y + name_h + 6, 44, 140};
    g.fill_rrect(bar, 9, TRACK);
    for (float f : {0.25f, 0.5f, 0.75f}) {
        float y = bar.y + std::round(bar.h * (1.0f - f));
        g.line(bar.x + 8, y, bar.right() - 8, y, mix(TRACK, BORDER_HI, 0.55));
    }
    float h = static_cast<float>(std::lrint(bar.h * value_));
    if (h >= 1) {
        // slupek przyciety do zaokraglonego rowka
        ComPtr<ID2D1Layer> layer;
        g.target()->CreateLayer(&layer);
        g.target()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),
                                                    g.geometry(Shape::rrect(bar.x, bar.y, bar.right(), bar.bottom(), 9)).Get()),
                              layer.Get());
        g.fill_rect({bar.x, bar.bottom() - h, bar.w, h}, color_);
        g.target()->PopLayer();
    }
    float y = bar.bottom() + 4;
    float pct_h = g.measure("Ag", Font::Heading).height;
    if (enabled_) g.text({r.x, y, r.w, pct_h}, std::format("{:.0f} %", value_ * 100), Font::Heading, value_ > 0.002 ? TEXT : MUTED, Align::Center);
    else g.text({r.x, y, r.w, pct_h}, "wył.", Font::Heading, MUTED, Align::Center);
    if (raw_) g.text({r.x, y + pct_h, r.w, r.bottom() - y - pct_h}, std::to_string(*raw_), Font::MonoSmall, MUTED, Align::Center);
}

void ButtonGrid::set_mask(uint32_t mask) {
    if (mask_ != mask) {
        mask_ = mask;
        invalidate();
    }
}

Size ButtonGrid::measure(float avail_w) { return {avail_w > 0 ? avail_w : 352, 4 * 21 + 3 * 4}; }

void ButtonGrid::paint(Gfx& g) {
    const int cols = 8;
    float gap = 4, step = (r.w - gap * (cols - 1)) / cols;
    for (int i = 0; i < 32; ++i) {
        int col = i % cols, row = i / cols;
        float x1 = std::round(static_cast<float>(col) * (step + gap));
        float x2 = std::round(static_cast<float>(col) * (step + gap) + step);
        Rect cell{r.x + x1, r.y + static_cast<float>(row) * 25, std::max(4.0f, x2 - x1), 21};
        bool on = (mask_ >> i) & 1;
        g.fill_rrect(cell, 5, on ? ACCENT : TRACK);
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
        p.fill(Shape::circle(c, c, c - 2), darken(TRACK, 0.35));
        std::vector<Pt> arrows[4] = {{{c, c - o}, {c - w, c - b}, {c + w, c - b}},
                                     {{c + o, c}, {c + b, c - w}, {c + b, c + w}},
                                     {{c, c + o}, {c - w, c + b}, {c + w, c + b}},
                                     {{c - o, c}, {c - b, c - w}, {c - b, c + w}}};
        for (int i = 0; i < 4; ++i) {
            bool on = i == dir;
            if (on) p.glow(Shape::poly(arrows[i]), ACCENT, 4, 120);
            p.fill(Shape::poly(arrows[i]), on ? ACCENT : TRACK);
        }
        p.fill(Shape::circle(c, c, gr * 0.75f), BORDER);
        p.fill(Shape::circle(c, c, gr * 0.75f - 1), PANEL_HI);
        return p.finish();
    });
    g.draw_sprite(sprite, r.x, r.y);
}

void Sparkline::push(double value, double scale_max) {
    data_.push_back(std::max(0.0, value));
    while (data_.size() > POINTS) data_.pop_front();
    top_ = std::max(scale_max, 1e-6);
    invalidate();
}

void Sparkline::paint(Gfx& g) {
    g.fill_rect(r, PANEL_HI);
    RGB grid = mix(PANEL_HI, BORDER, 0.8);
    for (float f : {0.33f, 0.66f}) g.line(r.x, r.y + r.h * f, r.right(), r.y + r.h * f, grid);
    g.line(r.x, r.bottom() - 0.5f, r.right(), r.bottom() - 0.5f, BORDER);
    if (data_.size() < 2) return;
    float step = r.w / static_cast<float>(POINTS - 1);
    size_t offset = POINTS - data_.size();
    std::vector<Pt> line;
    line.reserve(data_.size());
    for (size_t i = 0; i < data_.size(); ++i) {
        float x = r.x + static_cast<float>(offset + i) * step;
        float y = r.bottom() - 2 - static_cast<float>(std::min(data_[i], top_) / top_) * (r.h - 6);
        line.push_back({x, y});
    }
    std::vector<Pt> area;
    area.push_back({line.front().x, r.bottom()});
    area.insert(area.end(), line.begin(), line.end());
    area.push_back({line.back().x, r.bottom()});
    g.fill_poly(area, mix(PANEL_HI, color_, 0.22));
    g.polyline(line, color_, 1.8f);
}

// -------------------------------------------------------------- CurvePlot

CurvePlot::CurvePlot(float height, RGB color, bool bipolar, std::array<std::string, 3> captions)
    : height_(height), color_(color), bipolar_(bipolar), captions_(std::move(captions)) {}

void CurvePlot::set_curve(const std::string& key, std::function<double(double)> fn, std::vector<std::pair<double, double>> shade) {
    fn_ = std::move(fn);
    shade_ = std::move(shade);
    if (key != key_) {
        key_ = key;
        invalidate();
    }
}

void CurvePlot::set_marker(std::optional<std::pair<double, double>> xy) {
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

void CurvePlot::set_captions(const std::string& left, const std::string& middle, const std::string& right) {
    if (captions_[0] != left || captions_[1] != middle || captions_[2] != right) {
        captions_ = {left, middle, right};
        invalidate();
    }
}

Pt CurvePlot::to_px(double x, double y, float w) const {
    double lo = bipolar_ ? -1.0 : 0.0;
    const float inset = 5;
    float pw = w - 2 * inset, ph = height_ - 2 * inset;
    double tx = (x - lo) / (1.0 - lo), ty = (y - lo) / (1.0 - lo);
    return {inset + static_cast<float>(tx) * pw, inset + static_cast<float>(1.0 - ty) * ph};
}

Sprite CurvePlot::render(float width) {
    int w = static_cast<int>(width);
    float fw = static_cast<float>(w), h = height_;
    double lo = bipolar_ ? -1.0 : 0.0;
    const float pad = 1;
    Paint p(w, static_cast<int>(h));
    p.fill(Shape::rrect(0, 0, fw, h, 7), BORDER);
    p.fill(Shape::rrect(pad, pad, fw - pad, h - pad, 6), PANEL_HI);

    // zacienione martwe strefy - pas na cala wysokosc pola
    for (auto [a, b] : shade_) {
        if (b - a < 0.002) continue;
        float x1 = std::max(pad + 1, to_px(a, 0, fw).x);
        float x2 = std::min(fw - pad - 1, to_px(b, 0, fw).x);
        if (x2 - x1 >= 1) p.fill(Shape::rrect(x1, pad + 1, x2, h - pad - 1, 3), mix(PANEL_HI, BAD, 0.12));
    }

    RGB grid = mix(PANEL_HI, BORDER_HI, 0.55);
    for (double f : {0.25, 0.5, 0.75}) {
        double v = lo + (1.0 - lo) * f;
        float x = to_px(v, 0, fw).x, y = to_px(0, v, fw).y;
        RGB color = (bipolar_ && f == 0.5) ? BORDER_HI : grid;
        p.line({{x, pad + 2}, {x, h - pad - 2}}, color, 1.0f, 255, false);
        p.line({{pad + 2, y}, {fw - pad - 2, y}}, color, 1.0f, 255, false);
    }
    // przekatna - tak wyglada odpowiedz liniowa
    p.line({to_px(lo, lo, fw), to_px(1.0, 1.0, fw)}, mix(PANEL_HI, TEXT_DIM, 0.30), 1.0f);

    const int samples = 120;
    std::vector<Pt> pts;
    pts.reserve(samples + 1);
    for (int i = 0; i <= samples; ++i) {
        double x = lo + (1.0 - lo) * i / samples;
        pts.push_back(to_px(x, fn_ ? fn_(x) : 0.0, fw));
    }
    float base_y = to_px(0, 0, fw).y;
    std::vector<Pt> area;
    area.push_back({pts.front().x, base_y});
    area.insert(area.end(), pts.begin(), pts.end());
    area.push_back({pts.back().x, base_y});
    p.fill(Shape::poly(area), color_, 38);
    p.line(pts, color_, 2.2f);
    return p.finish();
}

void CurvePlot::paint(Gfx& g) {
    if (r.w < 20) return;
    float w = std::floor(r.w);
    if (fn_ && (drawn_key_ != key_ || drawn_w_ != w || drawn_gen_ != g.generation() || !sprite_)) {
        sprite_ = render(w);
        drawn_key_ = key_;
        drawn_w_ = w;
        drawn_gen_ = g.generation();
    }
    g.draw_sprite(sprite_, r.x, r.y);
    Rect caption{r.x + 1, r.y + height_ + 1, r.w - 2, 18};
    g.text(caption, captions_[0], Font::Tiny, MUTED);
    g.text(caption, captions_[1], Font::Tiny, MUTED, Align::Center);
    g.text(caption, captions_[2], Font::Tiny, MUTED, Align::Right);
    if (marker_) {
        Pt m = to_px(marker_->first, marker_->second, w);
        float x = r.x + std::round(m.x), y = r.y + std::round(m.y);
        g.fill_circle(x, y, 8, color_, 70.0f / 255.0f);
        g.fill_circle(x, y, 4.5f, color_);
        g.fill_circle(x, y, 3, TEXT);
    }
}

// -------------------------------------------------------------- WheelDial

void WheelDial::set_range(double degrees) {
    degrees = std::max(1.0, degrees);
    if (std::abs(degrees - range_) >= 0.5) {
        range_ = degrees;
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
    float s = size_;
    float ring_w = std::max(9.0f, std::floor(s / 19));
    float r_in = s / 2 - 6 - ring_w;
    float rad = r_in - 20;
    float rim_w = std::max(7.0f, s / 26);
    int half = static_cast<int>(std::ceil(rad + rim_w)) + 4;
    Paint p(half * 2, half * 2);
    float c = static_cast<float>(half);
    Shape rim = Shape::ring(c, c, rad + rim_w / 2, rad - rim_w / 2, 0, 360);
    // cien obreczy bez przesuniecia - obraca sie razem z kolem
    p.shadow(rim, 0, 4, 150);
    // ramiona w lewo i w prawo i szprycha w dol - pod obrecza
    RGB spoke = mix(PANEL_HI, TEXT, 0.16);
    for (float deg : {0.0f, 180.0f, 90.0f}) {
        float a = deg * PI / 180.0f;
        p.line({{c, c}, {c + rad * std::cos(a), c + rad * std::sin(a)}}, spoke, rim_w + 3, 255, false);
    }
    p.gradient(rim, mix(PANEL_HI, TEXT, 0.30), darken(PANEL_HI, 0.25));
    p.sheen(rim, 40, 0.4f);
    float hub = std::max(14.0f, s / 14);
    p.fill(Shape::circle(c, c, hub + 4), PANEL_HI);
    p.gradient(Shape::circle(c, c, hub), 0x2a313d, PANEL);
    p.stroke(Shape::circle(c, c, hub), mix(ACCENT_DIM, marker, 0.5), 2.0f);
    // pasek "gora kierownicy" na obreczy
    p.line({{c, c - rad - rim_w / 2 + 1}, {c, c - rad + rim_w / 2 - 1}}, marker, rim_w + 1, 255, false);
    return p.finish();
}

void WheelDial::paint(Gfx& g) {
    float s = size_;
    float c = s / 2;
    float ring_w = std::max(9.0f, std::floor(s / 19));
    float r_out = s / 2 - 6;
    float r_in = r_out - ring_w;
    float r_mid = r_out - ring_w / 2;
    float cx = r.x + c, cy = r.y + c;

    // tlo: rowek pierscienia, podzialka i znacznik zera
    const Sprite& base = g.cached("dial/" + std::to_string(static_cast<int>(s)), [&] {
        Paint p(static_cast<int>(s), static_cast<int>(s));
        p.fill(Shape::ring(c, c, r_out, r_in, 0, 360), TRACK);
        p.stroke(Shape::circle(c, c, r_out), BLACK, 1.0f, 60);
        for (int i = -4; i <= 4; ++i) {
            float a = (-90.0f + static_cast<float>(i) * 45.0f) * PI / 180.0f;
            bool major = (i & 1) == 0;
            float r1 = r_in - 5, r0 = r1 - (major ? 10.0f : 5.0f);
            p.line({{c + r0 * std::cos(a), c + r0 * std::sin(a)}, {c + r1 * std::cos(a), c + r1 * std::sin(a)}},
                   major ? TEXT_DIM : BORDER_HI, major ? 2.0f : 1.2f);
        }
        p.fill(Shape::poly({{c, c - r_out + 1}, {c - 5, c - r_out - 6}, {c + 5, c - r_out - 6}}), MUTED);
        return p.finish();
    });
    g.draw_sprite(base, r.x, r.y);

    double half = range_ / 2.0;
    double frac = std::max(-1.0, std::min(1.0, deg_ / half));
    bool over = std::abs(deg_) > half + 0.5;
    // kolor ma kilka stopni - kolo dla kazdego rysujemy tylko raz
    double near_end = std::rint(std::min(1.0, std::max(0.0, (std::abs(frac) - 0.75) / 0.25)) * 4) / 4;
    RGB color = over ? BAD : mix(ACCENT, WARN, near_end);

    if (std::abs(frac) > 0.003) {
        float a0 = -90.0f, a1 = -90.0f + static_cast<float>(frac) * 180.0f;
        if (a0 > a1) std::swap(a0, a1);
        g.arc(cx, cy, r_mid, a0, a1, color, ring_w - 2);
    }

    const Sprite& wheel = g.cached(std::format("dial-wheel/{}/{:06x}", static_cast<int>(s), color), [&] { return wheel_sprite(color); });
    g.draw_sprite_rotated(wheel, cx, cy, static_cast<float>(deg_));

    std::string text = std::abs(deg_) < 0.5 ? "0°" : std::format("{:+.0f}°", deg_);
    float ry = r.y + s + 6;
    g.text_at(cx, ry + 16, text, Font::Big, over ? BAD : TEXT);
    g.text_at(cx, ry + 46, std::format("zakres {}°", static_cast<int>(range_)), Font::Small, MUTED);
}

// --------------------------------------------------------------- TextView

float TextView::line_height() { return Gfx::get().measure("Ag", Font::MonoSmall).height; }

Size TextView::measure(float avail_w) { return {avail_w, static_cast<float>(rows_) * line_height() + 16}; }

void TextView::clear() {
    lines_.clear();
    offset_ = 0;
    invalidate();
}

void TextView::set_lines(const std::vector<std::string>& lines, RGB color) {
    lines_.clear();
    for (const auto& l : lines) lines_.push_back({l, color});
    invalidate();
}

void TextView::append(const std::string& line, RGB color) {
    lines_.push_back({line, color});
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

std::string TextView::joined() const {
    std::string out;
    for (size_t i = 0; i < lines_.size(); ++i) {
        if (i) out += "\n";
        out += lines_[i].text;
    }
    return out;
}

bool TextView::mouse_wheel(int delta) {
    float view = r.h - 16;
    if (content_h_ <= view) return false;
    float max_off = content_h_ - view;
    follow_ = false;
    offset_ = clampf(offset_ - static_cast<float>(delta) / 120.0f * 3 * line_height(), 0, max_off);
    if (offset_ >= max_off - 1) follow_ = true;
    invalidate();
    return true;
}

void TextView::paint(Gfx& g) {
    g.fill_rect(r, PANEL_HI);
    Rect in{r.x + 10, r.y + 8, r.w - 20 - 8, r.h - 16};
    float total = 0;
    std::vector<float> heights;
    heights.reserve(lines_.size());
    float line_h = line_height();
    for (const auto& l : lines_) {
        float h = l.text.empty() ? line_h : g.measure(l.text, Font::MonoSmall, in.w).height;
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
        if (y + heights[i] >= in.y && y <= in.bottom())
            g.text({in.x, y, in.w, heights[i]}, lines_[i].text, Font::MonoSmall, lines_[i].color, Align::Left, true);
        y += heights[i];
    }
    g.pop_clip();

    if (total > in.h) {
        float bar_h = std::max(20.0f, in.h * in.h / total);
        float bar_y = in.y + (in.h - bar_h) * (max_off > 0 ? offset_ / max_off : 0);
        g.fill_rrect({r.right() - 8, bar_y, 6, bar_h}, 3, ELEV);
    }
}

// ----------------------------------------------------------------- TabBar

TabBar::TabBar(std::vector<std::string> labels, std::vector<std::string> icons, std::function<void(int)> command)
    : labels_(std::move(labels)), icons_(std::move(icons)), command_(std::move(command)) {
    rects_.resize(labels_.size());
}

void TabBar::select(int index) {
    if (selected_ != index) {
        selected_ = index;
        invalidate();
    }
}

Size TabBar::measure(float) {
    Gfx& g = Gfx::get();
    float w = 0;
    float icon_w = g.has_icon_font() ? 15.0f + 7.0f : 0.0f;
    for (const auto& label : labels_) w += g.measure(label, Font::SmallBold).width + icon_w + 28;
    return {w, g.measure("Ag", Font::SmallBold).height + 8 + 9 + 2};
}

void TabBar::arrange(const Rect& rc) {
    r = rc;
    Gfx& g = Gfx::get();
    float x = rc.x;
    float icon_w = g.has_icon_font() ? 15.0f + 7.0f : 0.0f;
    for (size_t i = 0; i < labels_.size(); ++i) {
        float w = g.measure(labels_[i], Font::SmallBold).width + icon_w + 28;
        rects_[i] = {x, rc.y, w, rc.h};
        x += w;
    }
}

void TabBar::paint(Gfx& g) {
    for (size_t i = 0; i < labels_.size(); ++i) {
        bool sel = static_cast<int>(i) == selected_;
        RGB fg = sel ? TEXT : (static_cast<int>(i) == hover_ ? TEXT_DIM : MUTED);
        RGB icon_fg = sel ? ACCENT : fg;
        const Rect& rc = rects_[i];
        float x = rc.x + 14;
        Rect row{x, rc.y, rc.w - 28, rc.h - 2};
        if (g.has_icon_font()) {
            g.text({x, rc.y, 15, rc.h - 2}, icons_[i], Font::Icon, icon_fg, Align::Center);
            row.x += 22;
            row.w -= 22;
        }
        g.text(row, labels_[i], Font::SmallBold, fg);
        if (sel) g.fill_rect({rc.x + 6, rc.bottom() - 2, rc.w - 12, 2}, ACCENT);
    }
}

int TabBar::item_at(float x, float y) const {
    for (size_t i = 0; i < rects_.size(); ++i)
        if (rects_[i].contains(x, y)) return static_cast<int>(i);
    return -1;
}

void TabBar::mouse_move(float x, float y) {
    int i = item_at(x, y);
    if (i != hover_) {
        hover_ = i;
        invalidate();
    }
}

void TabBar::mouse_leave() {
    hover_ = -1;
    invalidate();
}

void TabBar::mouse_down(float x, float y) {
    int i = item_at(x, y);
    if (i >= 0 && command_) command_(i);
}

// -------------------------------------------------------------- PopupMenu

class MenuOverlay final : public Widget {
public:
    MenuOverlay(std::string title, std::vector<PopupMenu::Item> items, float x, float y)
        : title_(std::move(title)), items_(std::move(items)) {
        Gfx& g = Gfx::get();
        float width = 180;
        for (const auto& it : items_) {
            if (it.separator) continue;
            float w = 24 + g.measure(it.label, Font::SmallBold).width + 24;
            if (!it.note.empty()) w += g.measure(it.note, Font::Tiny).width + 12;
            width = std::max(width, w);
        }
        if (!title_.empty()) width = std::max(width, g.measure(title_, Font::TinyBold).width + 26);
        float height = 2 + (title_.empty() ? 0.0f : TITLE_H + 1);
        for (const auto& it : items_) height += it.separator ? 7.0f : ROW;
        Size win = Ui::get().window;
        // menu zostaje na ekranie, takze przy kliknieciu przy krawedzi
        box_ = {std::max(0.0f, std::min(x, win.w - width - 4)), std::max(0.0f, std::min(y, win.h - height - 4)), width, height};
        r = {0, 0, win.w, win.h};
    }

    Size measure(float) override { return Ui::get().window; }
    Widget* hit_test(float, float) override { return this; }
    bool hand_cursor() const override { return hover_ >= 0; }

    void paint(Gfx& g) override {
        g.fill_rect(box_, BORDER_HI);
        Rect in{box_.x + 1, box_.y + 1, box_.w - 2, box_.h - 2};
        g.fill_rect(in, PANEL_HI);
        float y = in.y;
        if (!title_.empty()) {
            g.text({in.x + 12, y, in.w - 24, TITLE_H}, title_, Font::TinyBold, MUTED);
            y += TITLE_H;
            g.fill_rect({in.x, y, in.w, 1}, BORDER);
            y += 1;
        }
        for (size_t i = 0; i < items_.size(); ++i) {
            const auto& it = items_[i];
            if (it.separator) {
                g.fill_rect({in.x, y + 3, in.w, 1}, BORDER);
                y += 7;
                continue;
            }
            Rect row{in.x, y, in.w, ROW};
            if (static_cast<int>(i) == hover_) g.fill_rect(row, ACCENT_DEEP);
            if (it.checked) g.text({row.x + 6, y, 18, ROW}, "✓", Font::SmallBold, ACCENT, Align::Center);
            g.text({row.x + 26, y, row.w - 38, ROW}, it.label, it.checked ? Font::SmallBold : Font::Small, TEXT);
            if (!it.note.empty()) g.text({row.x, y, row.w - 12, ROW}, it.note, Font::Tiny, WARN, Align::Right);
            y += ROW;
        }
    }

    int item_at(float px, float py) const {
        if (!box_.contains(px, py)) return -1;
        float y = box_.y + 1 + (title_.empty() ? 0.0f : TITLE_H + 1);
        for (size_t i = 0; i < items_.size(); ++i) {
            float h = items_[i].separator ? 7.0f : ROW;
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
    static constexpr float ROW = 26;
    static constexpr float TITLE_H = 26;
    std::string title_;
    std::vector<PopupMenu::Item> items_;
    Rect box_;
    int hover_ = -1;
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

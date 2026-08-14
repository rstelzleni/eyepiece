#include "viewport.h"

#include <algorithm>
#include <cmath>

namespace eye {
namespace {

constexpr double kMinZoom = 1.0 / 256.0;
constexpr double kMaxZoom = 512.0;
constexpr double kSnapEpsilon = 1e-6;

}  // namespace

void Viewport::set_window(int w, int h) {
    win_w_ = std::max(1, w);
    win_h_ = std::max(1, h);
}

void Viewport::set_image(int w, int h) {
    img_w_ = std::max(1, w);
    img_h_ = std::max(1, h);
}

void Viewport::set_zoom(double z) {
    zoom_ = std::clamp(z, kMinZoom, kMaxZoom);
}

void Viewport::set_content_inset(double left, double top, double right,
                                 double bottom) {
    inset_l_ = std::max(0.0, left);
    inset_t_ = std::max(0.0, top);
    inset_r_ = std::max(0.0, right);
    inset_b_ = std::max(0.0, bottom);
}

double Viewport::content_width() const {
    return std::max(1.0, win_w_ - inset_l_ - inset_r_);
}

double Viewport::content_height() const {
    return std::max(1.0, win_h_ - inset_t_ - inset_b_);
}

Vec2 Viewport::content_center() const {
    return {inset_l_ + content_width() * 0.5, inset_t_ + content_height() * 0.5};
}

void Viewport::fit() {
    const double sx = content_width() / img_w_;
    const double sy = content_height() / img_h_;
    set_zoom(std::min(sx, sy));
    cx_ = img_w_ * 0.5;
    cy_ = img_h_ * 0.5;
}

void Viewport::zoom_1to1() {
    set_zoom(1.0);
}

void Viewport::pan(double dx, double dy) {
    const double z = zoom_;
    cx_ -= dx / z;
    cy_ -= dy / z;
}

void Viewport::zoom_at(double factor, double sx, double sy) {
    const Vec2 anchor = screen_to_image({sx, sy});
    set_zoom(zoom_ * factor);
    // Re-solve the center so `anchor` lands back under the cursor:
    //   s = center + (i - c) * z   =>   c = i - (s - center) / z
    const Vec2 center = content_center();
    cx_ = anchor.x - (sx - center.x) / zoom_;
    cy_ = anchor.y - (sy - center.y) / zoom_;
}

void Viewport::zoom_step(int steps, double sx, double sy) {
    if (steps == 0) return;
    // Step along a power-of-two ladder rather than a fixed multiplier. This
    // guarantees stepped zoom lands on exact integer factors (or exact 1/2^n),
    // which are the only ones where pixels stay square without resampling.
    const double lg = std::log2(zoom_);
    const double target = (steps > 0) ? std::floor(lg + kSnapEpsilon) + steps
                                      : std::ceil(lg - kSnapEpsilon) + steps;
    const double want = std::clamp(std::exp2(target), kMinZoom, kMaxZoom);
    zoom_at(want / zoom_, sx, sy);
}

Rect Viewport::image_rect() const {
    const Vec2 center = content_center();
    double z = zoom_;
    double x0 = center.x + (0.0 - cx_) * z;
    double y0 = center.y + (0.0 - cy_) * z;

    // At (near-)integer zoom of 1:1 or greater, land the image on whole device
    // pixels. Without this the quad straddles pixel centers and even a perfect
    // GL_NEAREST sample produces visible seams where the rounding flips.
    if (snap_to_pixel_grid && z >= 1.0 &&
        std::abs(z - std::round(z)) < kSnapEpsilon) {
        z = std::round(z);
        x0 = std::round(x0);
        y0 = std::round(y0);
    }

    return {x0, y0, x0 + img_w_ * z, y0 + img_h_ * z};
}

double Viewport::effective_zoom() const {
    return image_rect().width() / img_w_;
}

Vec2 Viewport::screen_to_image(Vec2 s) const {
    const Rect r = image_rect();
    const double z = r.width() / img_w_;
    return {(s.x - r.x0) / z, (s.y - r.y0) / z};
}

Vec2 Viewport::image_to_screen(Vec2 i) const {
    const Rect r = image_rect();
    const double z = r.width() / img_w_;
    return {r.x0 + i.x * z, r.y0 + i.y * z};
}

}  // namespace eye

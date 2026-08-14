#pragma once

namespace eye {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

// Screen-space rectangle the image occupies, in device pixels.
struct Rect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
};

// Pan/zoom state and the screen<->image mapping.
//
// Everything that needs to agree about where a pixel landed -- the renderer,
// the probe, the pixel grid -- goes through image_rect(). That rect is snapped
// to whole device pixels at integer zoom, which is what keeps zoomed-in pixels
// exactly square with no half-pixel seams.
class Viewport {
public:
    void set_window(int w, int h);
    void set_image(int w, int h);

    // Region of the window not covered by UI chrome. Fitting and centering use
    // this rather than the raw window, so an image fit to the window is not
    // half-hidden behind the side panel.
    void set_content_inset(double left, double top, double right, double bottom);
    double content_width() const;
    double content_height() const;

    double zoom() const { return zoom_; }
    void set_zoom(double z);

    // Fit the whole image in the window. Does not clamp to 1:1 -- fitting a
    // 64x64 icon should fill the window.
    void fit();
    void zoom_1to1();

    void pan(double dx, double dy);                          // device pixels
    void zoom_at(double factor, double sx, double sy);       // anchored zoom
    void zoom_step(int steps, double sx, double sy);

    Vec2 screen_to_image(Vec2 s) const;
    Vec2 image_to_screen(Vec2 i) const;
    Rect image_rect() const;

    // Effective zoom after snapping -- what is actually on screen.
    double effective_zoom() const;

    bool snap_to_pixel_grid = true;

    int window_width() const { return win_w_; }
    int window_height() const { return win_h_; }
    int image_width() const { return img_w_; }
    int image_height() const { return img_h_; }

private:
    double zoom_ = 1.0;
    double cx_ = 0.0, cy_ = 0.0;  // image coords sitting at the window center
    int win_w_ = 1, win_h_ = 1;
    int img_w_ = 1, img_h_ = 1;
    double inset_l_ = 0.0, inset_t_ = 0.0, inset_r_ = 0.0, inset_b_ = 0.0;

    Vec2 content_center() const;
};

}  // namespace eye

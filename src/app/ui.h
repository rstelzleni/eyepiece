#pragma once

#include <string>

#include "core/color.h"
#include "core/session.h"
#include "core/viewport.h"
#include "gpu/renderer.h"

namespace eye {

inline constexpr float kPanelWidth = 340.0f;
// Only used to seed the first fit, before ImGui has measured a real frame.
inline constexpr float kStatusBarHeightEstimate = 30.0f;

// Everything the UI reads or writes. Deliberately a plain struct: the panels
// are the only thing that should ever grow policy, and keeping state dumb makes
// the eventual library split a matter of deleting app/ rather than untangling it.
struct AppState {
    Session session;
    Viewport viewport;
    ColorManager color;
    DrawOptions draw;

    // Probe, refilled every frame from the CPU-side buffer.
    bool probe_valid = false;
    int probe_x = 0;
    int probe_y = 0;
    float probe_source[4] = {0, 0, 0, 0};
    float probe_display[4] = {0, 0, 0, 0};
    bool probe_display_valid = false;

    bool show_panel = true;
    bool show_pixel_values = true;
    std::string status;
    std::string error;
    float gamma = 1.0f;
    float exposure = 0.0f;

    bool image_changed = false;  // renderer should re-upload
};

// Refreshes probe_* from the image under the cursor.
void update_probe(AppState& s, double mouse_x, double mouse_y);

// Side panel + status bar.
void draw_ui(AppState& s);

// Numeric values drawn inside each pixel cell once zoom is high enough.
void draw_pixel_value_overlay(const AppState& s);

}  // namespace eye

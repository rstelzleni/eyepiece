#include "app/ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

namespace eye {
namespace {

constexpr ImGuiWindowFlags kFixedPanel =
    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;

// Values below this zoom cannot fit legible text inside one pixel cell.
constexpr double kPixelTextMinZoom = 48.0;
constexpr int kPixelTextMaxCells = 4000;

std::string format_float(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return buf;
}

bool string_combo(const char* label, const std::vector<std::string>& items,
                  const std::string& current, std::string* chosen) {
    bool changed = false;
    if (ImGui::BeginCombo(label, current.c_str())) {
        for (const auto& item : items) {
            const bool selected = (item == current);
            if (ImGui::Selectable(item.c_str(), selected)) {
                *chosen = item;
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

}  // namespace

void update_probe(AppState& s, double mouse_x, double mouse_y) {
    s.probe_valid = false;
    s.probe_display_valid = false;

    const ImagePtr img = s.session.current();
    if (!img) return;

    const Vec2 ip = s.viewport.screen_to_image({mouse_x, mouse_y});
    const int x = static_cast<int>(std::floor(ip.x));
    const int y = static_cast<int>(std::floor(ip.y));

    const float* px = img->pixel(x, y);
    if (!px) return;

    s.probe_x = x;
    s.probe_y = y;
    std::memcpy(s.probe_source, px, sizeof(float) * 4);

    float tmp[4] = {px[0], px[1], px[2], px[3]};
    s.probe_display_valid = s.color.apply_display(tmp);
    std::memcpy(s.probe_display, tmp, sizeof(float) * 4);
    s.probe_valid = true;
}

void draw_ui(AppState& s) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float bar_h = ImGui::GetFrameHeight() + ImGui::GetStyle().WindowPadding.y * 2;
    const float panel_w = kPanelWidth;
    const ImagePtr img = s.session.current();

    // Tell the viewport what the UI is covering, so fit() and centering target
    // the visible area rather than the whole window.
    s.viewport.set_content_inset(0.0, 0.0, s.show_panel ? panel_w : 0.0, bar_h);

    // ---- status bar -------------------------------------------------------
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - bar_h));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, bar_h));
    ImGui::Begin("##status", nullptr, kFixedPanel | ImGuiWindowFlags_NoTitleBar);
    if (img) {
        ImGui::Text("%s", img->display_name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d %s", img->width, img->height,
                            img->format.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("| %.0f%%", s.viewport.effective_zoom() * 100.0);
        ImGui::SameLine();

        if (s.probe_valid) {
            ImGui::Text("| [%d, %d]", s.probe_x, s.probe_y);
            ImGui::SameLine();
            // Source values first: this is the number in the file, and it is
            // the one you are usually trying to read.
            ImGui::Text("src %s %s %s %s", format_float(s.probe_source[0]).c_str(),
                        format_float(s.probe_source[1]).c_str(),
                        format_float(s.probe_source[2]).c_str(),
                        format_float(s.probe_source[3]).c_str());
            if (s.probe_display_valid) {
                ImGui::SameLine();
                const int r = std::clamp(int(s.probe_display[0] * 255.0f + 0.5f), 0, 255);
                const int g = std::clamp(int(s.probe_display[1] * 255.0f + 0.5f), 0, 255);
                const int b = std::clamp(int(s.probe_display[2] * 255.0f + 0.5f), 0, 255);
                ImGui::TextDisabled("| disp #%02X%02X%02X", r, g, b);
                ImGui::SameLine();
                ImGui::ColorButton(
                    "##swatch",
                    ImVec4(s.probe_display[0], s.probe_display[1],
                           s.probe_display[2], 1.0f),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
            }
        } else {
            ImGui::TextDisabled("| --");
        }
    } else {
        ImGui::TextDisabled("no image loaded  --  drop a file, or pass one on the command line");
    }
    ImGui::End();

    if (!s.show_panel) return;

    // ---- side panel -------------------------------------------------------
    ImGui::SetNextWindowPos(
        ImVec2(vp->WorkPos.x + vp->WorkSize.x - panel_w, vp->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(panel_w, vp->WorkSize.y - bar_h));
    ImGui::Begin("eyepiece", nullptr, kFixedPanel);

    if (!s.error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", s.error.c_str());
        ImGui::Separator();
    }

    // Each section gets its own ID scope. CollapsingHeader does not push one,
    // so without this every widget in the panel shares the window root scope --
    // which is how the OCIO "View" combo and the "View" section header ended up
    // hashing to the same ID.
    if (ImGui::CollapsingHeader("Color", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("color");
        ImGui::TextDisabled("%s", s.color.config_name().c_str());

        std::string chosen;
        if (string_combo("Input", s.color.colorspaces(),
                         s.color.input_colorspace(), &chosen))
            s.color.set_input_colorspace(chosen);
        if (string_combo("Display", s.color.displays(), s.color.display(), &chosen))
            s.color.set_display(chosen);
        if (string_combo("View", s.color.views(), s.color.view(), &chosen))
            s.color.set_view(chosen);

        if (ImGui::SliderFloat("Exposure", &s.exposure, -8.0f, 8.0f, "%.2f stops"))
            s.color.set_exposure(s.exposure);
        ImGui::SameLine();
        if (ImGui::SmallButton("0##exp")) {
            s.exposure = 0.0f;
            s.color.set_exposure(0.0f);
        }

        ImGui::SliderFloat("Gamma", &s.gamma, 0.1f, 4.0f, "%.2f");
        ImGui::SameLine();
        if (ImGui::SmallButton("1##gam")) s.gamma = 1.0f;
        s.draw.gamma = s.gamma;
        ImGui::PopID();
    }

    if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("view");
        const char* labels[] = {"RGB", "R", "G", "B", "A", "Luma"};
        int channel = static_cast<int>(s.draw.channel);
        if (ImGui::Combo("Channel", &channel, labels, IM_ARRAYSIZE(labels)))
            s.draw.channel = static_cast<ChannelView>(channel);

        ImGui::Checkbox("Checkerboard", &s.draw.checkerboard);
        ImGui::Checkbox("Pixel grid", &s.draw.pixel_grid);
        ImGui::Checkbox("Pixel values", &s.show_pixel_values);
        ImGui::Checkbox("Snap to pixel grid", &s.viewport.snap_to_pixel_grid);
        ImGui::PopID();
    }

    if (s.session.size() > 1 &&
        ImGui::CollapsingHeader("Images", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("images");
        for (size_t i = 0; i < s.session.size(); ++i) {
            // Index scope, not the name: opening two files with the same
            // basename from different directories is normal, and their
            // selectables would otherwise collide too.
            ImGui::PushID(static_cast<int>(i));
            const bool selected = (i == s.session.current_index());
            if (ImGui::Selectable(s.session.images()[i]->display_name.c_str(),
                                  selected)) {
                s.session.set_current(i);
                s.image_changed = true;
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }

    if (img && ImGui::CollapsingHeader("Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::PushID("metadata");
        ImGui::TextDisabled("colorspace: %s", img->colorspace.c_str());
        if (ImGui::BeginTable("meta", 2,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupColumn("key", ImGuiTableColumnFlags_WidthStretch, 0.42f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
            for (const auto& e : img->metadata) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(e.key.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("%s", e.value.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    ImGui::End();
}

void draw_pixel_value_overlay(const AppState& s) {
    if (!s.show_pixel_values) return;
    const ImagePtr img = s.session.current();
    if (!img) return;

    const double zoom = s.viewport.effective_zoom();
    if (zoom < kPixelTextMinZoom) return;

    const Vec2 tl = s.viewport.screen_to_image({0.0, 0.0});
    const Vec2 br = s.viewport.screen_to_image(
        {static_cast<double>(s.viewport.window_width()),
         static_cast<double>(s.viewport.window_height())});

    const int x0 = std::max(0, static_cast<int>(std::floor(tl.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor(tl.y)));
    const int x1 = std::min(img->width, static_cast<int>(std::ceil(br.x)) + 1);
    const int y1 = std::min(img->height, static_cast<int>(std::ceil(br.y)) + 1);
    if (x1 <= x0 || y1 <= y0) return;
    if (static_cast<long>(x1 - x0) * (y1 - y0) > kPixelTextMaxCells) return;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float font_size =
        static_cast<float>(std::clamp(zoom / 5.0, 8.0, 15.0));

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const float* p = img->pixel(x, y);
            if (!p) continue;

            // Rough perceptual luma of the source is enough to decide whether
            // black or white text will read against the cell behind it.
            const float luma = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
            const ImU32 col = (std::pow(std::max(luma, 0.0f), 1.0f / 2.2f) > 0.55f)
                                  ? IM_COL32(0, 0, 0, 210)
                                  : IM_COL32(255, 255, 255, 210);

            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.3f\n%.3f\n%.3f", p[0], p[1], p[2]);

            const Vec2 sp = s.viewport.image_to_screen(
                {static_cast<double>(x) + 0.08, static_cast<double>(y) + 0.08});
            dl->AddText(nullptr, font_size,
                        ImVec2(static_cast<float>(sp.x), static_cast<float>(sp.y)),
                        col, buf);
        }
    }
}

}  // namespace eye

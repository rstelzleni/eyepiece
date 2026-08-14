#pragma once

#include <string>
#include <vector>

#include <OpenColorIO/OpenColorIO.h>

#include "image.h"

namespace OCIO = OCIO_NAMESPACE;

namespace eye {

// Owns the OCIO config and turns (input colorspace, display, view) into GLSL
// source plus the LUT resources that shader needs.
//
// Exposure rides an OCIO *dynamic property* rather than being baked into the
// transform. That distinction matters: baking it would mean rebuilding the
// processor and recompiling the shader on every slider frame, which is tens of
// milliseconds of stall per drag. As a dynamic property it is just a uniform.
class ColorManager {
public:
    bool init(std::string* error);

    const std::vector<std::string>& displays() const { return displays_; }
    const std::vector<std::string>& views() const { return views_; }
    const std::vector<std::string>& colorspaces() const { return colorspaces_; }

    const std::string& display() const { return display_; }
    const std::string& view() const { return view_; }
    const std::string& input_colorspace() const { return input_cs_; }
    const std::string& config_name() const { return config_name_; }

    void set_display(const std::string& d);
    void set_view(const std::string& v);
    void set_input_colorspace(const std::string& cs);

    // Maps a loader hint onto a colorspace that actually exists in this config,
    // trying the usual spellings across ACES / nuke-default / builtin configs.
    std::string resolve_hint(ColorHint hint, const std::string& file_cs) const;

    // Exposure in stops, applied in scene-linear before the display transform.
    void set_exposure(float stops);
    float exposure() const { return exposure_; }

    // Rebuilds the GPU processor if anything structural changed. Cheap no-op
    // when nothing did.
    bool build(std::string* error);

    // Bumped whenever shader_desc() starts pointing at new source, so the
    // renderer knows to recompile and re-upload LUTs.
    unsigned generation() const { return generation_; }

    // The renderer consumes this directly rather than going through a neutral
    // intermediate: OCIO's descriptor already *is* the description of what
    // textures and uniforms to bind, and re-expressing it would only add drift.
    OCIO::GpuShaderDescRcPtr shader_desc() const { return desc_; }

    // Pushes the current dynamic values into the descriptor. Call once per
    // frame before binding uniforms.
    void update_dynamic();

    // Runs the same transform the GPU is showing, on the CPU, for one pixel.
    // This is what lets the probe report the source value and the displayed
    // value side by side instead of making you guess at the relationship.
    bool apply_display(float rgba[4]) const;

private:
    void refresh_views();

    OCIO::ConstConfigRcPtr config_;
    OCIO::GpuShaderDescRcPtr desc_;
    OCIO::ConstCPUProcessorRcPtr cpu_;

    std::string config_name_;
    std::string display_, view_, input_cs_, linear_cs_;
    std::vector<std::string> displays_, views_, colorspaces_;

    float exposure_ = 0.0f;
    bool structural_dirty_ = true;
    unsigned generation_ = 0;
};

}  // namespace eye

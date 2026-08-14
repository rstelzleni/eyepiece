#pragma once

#include <string>
#include <vector>

#include "core/color.h"
#include "core/image.h"
#include "core/viewport.h"

namespace eye {

enum class ChannelView { RGB, Red, Green, Blue, Alpha, Luma };

struct DrawOptions {
    ChannelView channel = ChannelView::RGB;
    float gamma = 1.0f;          // display-referred trim, applied after OCIO
    bool checkerboard = true;    // composite over a checker to reveal alpha
    bool pixel_grid = true;
    float grid_min_zoom = 6.0f;  // zoom at which the grid starts drawing
};

// Draws one image, color managed, with pixel-exact magnification.
class Renderer {
public:
    bool init(std::string* error);
    void shutdown();

    void set_image(const Image& img);
    void clear_image();

    // Recompiles against the current OCIO shader if its generation moved.
    bool draw(const Viewport& vp, ColorManager& color, const DrawOptions& opts,
              std::string* error);

private:
    bool rebuild_program(ColorManager& color, std::string* error);
    void release_ocio_textures();
    void upload_ocio_textures(const OCIO::GpuShaderDescRcPtr& desc);
    void bind_ocio_uniforms(const OCIO::GpuShaderDescRcPtr& desc);

    unsigned vao_ = 0;
    unsigned program_ = 0;
    unsigned image_tex_ = 0;
    int image_w_ = 0, image_h_ = 0;

    struct OcioTexture {
        unsigned id = 0;
        unsigned target = 0;
        std::string sampler;
    };
    std::vector<OcioTexture> ocio_textures_;

    unsigned color_generation_ = 0;
    bool have_program_ = false;
};

}  // namespace eye

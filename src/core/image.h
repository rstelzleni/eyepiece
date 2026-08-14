#pragma once

#include <memory>
#include <string>
#include <vector>

namespace eye {

struct MetadataEntry {
    std::string key;
    std::string value;
};

// What the loader believes the pixel values mean. The loader deliberately does
// not resolve this to an OCIO colorspace name: which names exist depends on the
// active config, so resolution belongs to the color module.
enum class ColorHint {
    Unknown,
    sRGB,         // display-referred, sRGB transfer applied (PNG, JPEG, ...)
    SceneLinear,  // scene-referred linear (EXR, HDR)
};

// A decoded image, always stored as float32 RGBA on the CPU.
//
// The CPU-side buffer is not a staging area -- it is kept for the lifetime of
// the image so the pixel probe can report true file values. Reading back from
// the GPU would only ever tell us what the display transform produced.
class Image {
public:
    int width = 0;
    int height = 0;
    int source_channels = 0;  // channel count as stored in the file

    std::vector<float> pixels;  // width * height * 4, RGBA, row-major, top-down

    std::string path;
    std::string display_name;   // basename, for tabs and window title
    std::string format;         // "openexr", "png", ...
    ColorHint hint = ColorHint::Unknown;
    std::string file_colorspace;  // colorspace named by the file itself, if any
    std::string colorspace;       // resolved OCIO colorspace, filled by color.cpp

    std::vector<std::string> channel_names;
    std::vector<MetadataEntry> metadata;

    bool valid() const { return width > 0 && height > 0 && !pixels.empty(); }

    bool contains(int x, int y) const {
        return x >= 0 && y >= 0 && x < width && y < height;
    }

    // Returns nullptr when out of bounds, so callers must check. The probe
    // hovers outside the image constantly; that is not an error worth throwing.
    const float* pixel(int x, int y) const {
        if (!contains(x, y)) return nullptr;
        return pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
    }
};

using ImagePtr = std::shared_ptr<Image>;

}  // namespace eye

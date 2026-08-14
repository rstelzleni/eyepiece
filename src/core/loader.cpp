#include "loader.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <sstream>

#ifdef EYEPIECE_WITH_OIIO
#include <OpenImageIO/imageio.h>
#else
#include <ImathBox.h>
#include <ImfArray.h>
#include <ImfHeader.h>
#include <ImfRgbaFile.h>
#include <ImfStringAttribute.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO_WRITE
#include "stb_image.h"
#endif

namespace eye {
namespace {

std::string lower_ext(const std::string& path) {
    std::string e = std::filesystem::path(path).extension().string();
    if (!e.empty() && e[0] == '.') e.erase(0, 1);
    std::transform(e.begin(), e.end(), e.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return e;
}

// Widens whatever channel count the file had into RGBA. Single-channel images
// replicate to grey rather than landing in red, which is what you want when
// inspecting a depth or alpha pass.
void expand_to_rgba(const float* src, int nchan, size_t npixels, float* dst) {
    for (size_t i = 0; i < npixels; ++i) {
        const float* s = src + i * nchan;
        float* d = dst + i * 4;
        switch (nchan) {
            case 1: d[0] = d[1] = d[2] = s[0]; d[3] = 1.0f; break;
            case 2: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            case 3: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 1.0f; break;
            default:
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
                break;
        }
    }
}

ColorHint hint_from_extension(const std::string& ext) {
    if (ext == "exr" || ext == "hdr" || ext == "pfm") return ColorHint::SceneLinear;
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" ||
        ext == "tga" || ext == "gif" || ext == "webp" || ext == "tif" ||
        ext == "tiff")
        return ColorHint::sRGB;
    return ColorHint::Unknown;
}

void finish(const ImagePtr& img, const std::string& path) {
    img->path = path;
    img->display_name = std::filesystem::path(path).filename().string();
    if (img->channel_names.empty()) {
        static const char* kDefault[] = {"R", "G", "B", "A"};
        for (int i = 0; i < std::min(img->source_channels, 4); ++i)
            img->channel_names.push_back(kDefault[i]);
    }
}

}  // namespace

#ifdef EYEPIECE_WITH_OIIO

const char* loader_backend() { return "OpenImageIO"; }

std::vector<std::string> supported_extensions() {
    std::vector<std::string> out;
    std::string list = OIIO::get_string_attribute("extension_list");
    // Format is "fmt:ext,ext;fmt:ext,ext;..."
    std::stringstream groups(list);
    std::string group;
    while (std::getline(groups, group, ';')) {
        const size_t colon = group.find(':');
        if (colon == std::string::npos) continue;
        std::stringstream exts(group.substr(colon + 1));
        std::string e;
        while (std::getline(exts, e, ',')) out.push_back(e);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

ImagePtr load_image(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& msg) -> ImagePtr {
        if (error) *error = msg;
        return nullptr;
    };

    auto in = OIIO::ImageInput::open(path);
    if (!in) return fail(OIIO::geterror());

    const OIIO::ImageSpec& spec = in->spec();
    if (spec.width <= 0 || spec.height <= 0 || spec.nchannels <= 0)
        return fail("image has no pixels");

    auto img = std::make_shared<Image>();
    img->width = spec.width;
    img->height = spec.height;
    img->source_channels = spec.nchannels;
    img->format = in->format_name();

    const size_t npixels = static_cast<size_t>(spec.width) * spec.height;
    std::vector<float> raw(npixels * spec.nchannels);
    if (!in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT,
                        raw.data())) {
        return fail(in->geterror());
    }
    in->close();

    img->pixels.resize(npixels * 4);
    expand_to_rgba(raw.data(), spec.nchannels, npixels, img->pixels.data());

    for (const auto& name : spec.channelnames) img->channel_names.push_back(name);

    img->metadata.push_back(
        {"resolution", std::to_string(spec.width) + " x " +
                           std::to_string(spec.height)});
    img->metadata.push_back({"channels", std::to_string(spec.nchannels)});
    img->metadata.push_back({"format", spec.format.c_str()});
    if (spec.x != 0 || spec.y != 0 || spec.full_width != spec.width ||
        spec.full_height != spec.height) {
        img->metadata.push_back(
            {"data window", std::to_string(spec.x) + "," + std::to_string(spec.y) +
                                " + " + std::to_string(spec.width) + "x" +
                                std::to_string(spec.height)});
        img->metadata.push_back({"display window",
                                 std::to_string(spec.full_width) + "x" +
                                     std::to_string(spec.full_height)});
    }
    for (const auto& p : spec.extra_attribs)
        img->metadata.push_back(
            {p.name().string(), OIIO::ImageSpec::metadata_val(p, true)});

    img->file_colorspace = spec.get_string_attribute("oiio:ColorSpace");
    const std::string cs = img->file_colorspace;
    if (cs.find("inear") != std::string::npos)
        img->hint = ColorHint::SceneLinear;
    else if (cs.find("sRGB") != std::string::npos || cs.find("srgb") != std::string::npos)
        img->hint = ColorHint::sRGB;
    else
        img->hint = hint_from_extension(lower_ext(path));

    finish(img, path);
    return img;
}

#else  // builtin backend

const char* loader_backend() { return "builtin (OpenEXR + stb_image)"; }

std::vector<std::string> supported_extensions() {
    return {"bmp", "exr", "gif", "hdr", "jpeg", "jpg", "pgm",
            "pic", "png", "pnm", "ppm", "psd", "tga"};
}

namespace {

std::string exr_attr_to_string(const Imf::Attribute& attr) {
    const std::string type = attr.typeName();
    std::ostringstream os;
    if (type == "string")
        return static_cast<const Imf::StringAttribute&>(attr).value();
    if (type == "int") {
        os << static_cast<const Imf::TypedAttribute<int>&>(attr).value();
        return os.str();
    }
    if (type == "float") {
        os << static_cast<const Imf::TypedAttribute<float>&>(attr).value();
        return os.str();
    }
    if (type == "v2f") {
        const auto& v = static_cast<const Imf::TypedAttribute<Imath::V2f>&>(attr).value();
        os << v.x << ", " << v.y;
        return os.str();
    }
    if (type == "box2i") {
        const auto& b = static_cast<const Imf::TypedAttribute<Imath::Box2i>&>(attr).value();
        os << b.min.x << "," << b.min.y << " .. " << b.max.x << "," << b.max.y;
        return os.str();
    }
    return "<" + type + ">";
}

ImagePtr load_exr(const std::string& path, std::string* error) {
    try {
        Imf::RgbaInputFile file(path.c_str());
        const Imath::Box2i dw = file.dataWindow();
        const int w = dw.max.x - dw.min.x + 1;
        const int h = dw.max.y - dw.min.y + 1;
        if (w <= 0 || h <= 0) {
            if (error) *error = "EXR data window is empty";
            return nullptr;
        }

        Imf::Array2D<Imf::Rgba> half_px(h, w);
        file.setFrameBuffer(&half_px[0][0] - dw.min.x - dw.min.y * w, 1, w);
        file.readPixels(dw.min.y, dw.max.y);

        auto img = std::make_shared<Image>();
        img->width = w;
        img->height = h;
        img->source_channels = 4;
        img->format = "openexr";
        img->hint = ColorHint::SceneLinear;
        img->pixels.resize(static_cast<size_t>(w) * h * 4);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
            const Imf::Rgba& s = (&half_px[0][0])[i];
            float* d = img->pixels.data() + i * 4;
            d[0] = static_cast<float>(s.r);
            d[1] = static_cast<float>(s.g);
            d[2] = static_cast<float>(s.b);
            d[3] = static_cast<float>(s.a);
        }

        img->metadata.push_back(
            {"resolution", std::to_string(w) + " x " + std::to_string(h)});
        for (auto it = file.header().begin(); it != file.header().end(); ++it)
            img->metadata.push_back({it.name(), exr_attr_to_string(it.attribute())});

        finish(img, path);
        return img;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return nullptr;
    }
}

ImagePtr load_stb(const std::string& path, std::string* error) {
    int w = 0, h = 0, n = 0;
    auto img = std::make_shared<Image>();
    const std::string ext = lower_ext(path);

    if (stbi_is_hdr(path.c_str())) {
        float* data = stbi_loadf(path.c_str(), &w, &h, &n, 0);
        if (!data) {
            if (error) *error = stbi_failure_reason();
            return nullptr;
        }
        img->pixels.resize(static_cast<size_t>(w) * h * 4);
        expand_to_rgba(data, n, static_cast<size_t>(w) * h, img->pixels.data());
        stbi_image_free(data);
        img->hint = ColorHint::SceneLinear;
    } else {
        // Deliberately NOT stbi_loadf here: for LDR input it silently applies a
        // 2.2 gamma, which would bake an unlabelled transform into values we
        // are about to report as ground truth. Read the integers and normalize.
        const bool is16 = stbi_is_16_bit(path.c_str());
        std::vector<float> raw;
        if (is16) {
            stbi_us* data = stbi_load_16(path.c_str(), &w, &h, &n, 0);
            if (!data) {
                if (error) *error = stbi_failure_reason();
                return nullptr;
            }
            raw.resize(static_cast<size_t>(w) * h * n);
            for (size_t i = 0; i < raw.size(); ++i) raw[i] = data[i] / 65535.0f;
            stbi_image_free(data);
        } else {
            stbi_uc* data = stbi_load(path.c_str(), &w, &h, &n, 0);
            if (!data) {
                if (error) *error = stbi_failure_reason();
                return nullptr;
            }
            raw.resize(static_cast<size_t>(w) * h * n);
            for (size_t i = 0; i < raw.size(); ++i) raw[i] = data[i] / 255.0f;
            stbi_image_free(data);
        }
        img->pixels.resize(static_cast<size_t>(w) * h * 4);
        expand_to_rgba(raw.data(), n, static_cast<size_t>(w) * h, img->pixels.data());
        img->hint = hint_from_extension(ext);
        img->metadata.push_back({"bit depth", is16 ? "16" : "8"});
    }

    img->width = w;
    img->height = h;
    img->source_channels = n;
    img->format = ext;
    img->metadata.insert(
        img->metadata.begin(),
        {{"resolution", std::to_string(w) + " x " + std::to_string(h)},
         {"channels", std::to_string(n)}});
    finish(img, path);
    return img;
}

}  // namespace

ImagePtr load_image(const std::string& path, std::string* error) {
    if (!std::filesystem::exists(path)) {
        if (error) *error = "no such file: " + path;
        return nullptr;
    }
    if (lower_ext(path) == "exr") return load_exr(path, error);
    return load_stb(path, error);
}

#endif  // EYEPIECE_WITH_OIIO

}  // namespace eye

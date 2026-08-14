#include "color.h"

#include <algorithm>
#include <cstdlib>

namespace eye {
namespace {

// Spellings for the same concept across the configs people actually have
// installed: ACES studio/cg configs, the OCIO builtin, and nuke-default.
const char* kLinearCandidates[] = {
    "lin_rec709_scene", "lin_rec709", "Linear Rec.709 (sRGB)",
    "scene_linear",     "ACEScg",     "lin_ap1_scene",
    "linear",
};

const char* kSrgbCandidates[] = {
    "srgb_tx",     "sRGB - Texture",  "Input - Generic - sRGB - Texture",
    "srgb_texture", "sRGB",           "Utility - sRGB - Texture",
    "color_picking",
};

bool has_colorspace(const OCIO::ConstConfigRcPtr& cfg, const char* name) {
    if (!cfg || !name || !*name) return false;
    // getColorSpace() resolves roles and aliases as well as literal names.
    return static_cast<bool>(cfg->getColorSpace(name));
}

std::string first_present(const OCIO::ConstConfigRcPtr& cfg,
                          const char* const* candidates, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (has_colorspace(cfg, candidates[i]))
            return cfg->getColorSpace(candidates[i])->getName();
    return {};
}

}  // namespace

bool ColorManager::init(std::string* error) {
    try {
        const char* env = std::getenv("OCIO");
        if (env && *env) {
            config_ = OCIO::Config::CreateFromEnv();
            config_name_ = env;
        } else {
            config_ = OCIO::Config::CreateFromBuiltinConfig("ocio://default");
            config_name_ = "ocio://default (builtin)";
        }
    } catch (const std::exception& e) {
        if (error) *error = std::string("failed to load OCIO config: ") + e.what();
        return false;
    }

    for (int i = 0; i < config_->getNumColorSpaces(); ++i)
        colorspaces_.push_back(config_->getColorSpaceNameByIndex(i));
    for (int i = 0; i < config_->getNumDisplays(); ++i)
        displays_.push_back(config_->getDisplay(i));

    if (displays_.empty()) {
        if (error) *error = "OCIO config defines no displays";
        return false;
    }

    display_ = config_->getDefaultDisplay();
    if (display_.empty()) display_ = displays_.front();
    refresh_views();
    view_ = config_->getDefaultView(display_.c_str());
    if (view_.empty() && !views_.empty()) view_ = views_.front();

    linear_cs_ = first_present(config_, kLinearCandidates,
                               std::size(kLinearCandidates));
    if (linear_cs_.empty()) {
        if (error)
            *error = "could not find a scene-linear colorspace in config " +
                     config_name_;
        return false;
    }
    input_cs_ = linear_cs_;
    structural_dirty_ = true;
    return true;
}

void ColorManager::refresh_views() {
    views_.clear();
    for (int i = 0; i < config_->getNumViews(display_.c_str()); ++i)
        views_.push_back(config_->getView(display_.c_str(), i));
}

void ColorManager::set_display(const std::string& d) {
    if (d == display_) return;
    display_ = d;
    refresh_views();
    // The previous view name usually does not exist under the new display.
    if (std::find(views_.begin(), views_.end(), view_) == views_.end()) {
        view_ = config_->getDefaultView(display_.c_str());
        if (view_.empty() && !views_.empty()) view_ = views_.front();
    }
    structural_dirty_ = true;
}

void ColorManager::set_view(const std::string& v) {
    if (v == view_) return;
    view_ = v;
    structural_dirty_ = true;
}

void ColorManager::set_input_colorspace(const std::string& cs) {
    if (cs == input_cs_) return;
    input_cs_ = cs;
    structural_dirty_ = true;
}

void ColorManager::set_exposure(float stops) {
    exposure_ = stops;  // dynamic; no rebuild
}

std::string ColorManager::resolve_hint(ColorHint hint,
                                       const std::string& file_cs) const {
    if (!file_cs.empty() && has_colorspace(config_, file_cs.c_str()))
        return config_->getColorSpace(file_cs.c_str())->getName();

    switch (hint) {
        case ColorHint::sRGB: {
            std::string s = first_present(config_, kSrgbCandidates,
                                          std::size(kSrgbCandidates));
            if (!s.empty()) return s;
            break;
        }
        case ColorHint::SceneLinear:
            return linear_cs_;
        case ColorHint::Unknown:
            break;
    }
    return linear_cs_;
}

bool ColorManager::build(std::string* error) {
    if (!structural_dirty_ && desc_) return true;

    try {
        auto group = OCIO::GroupTransform::Create();

        // Get into scene-linear first so the exposure control means "stops of
        // light", not "stops of whatever encoding this file happened to use".
        if (input_cs_ != linear_cs_) {
            auto to_linear = OCIO::ColorSpaceTransform::Create();
            to_linear->setSrc(input_cs_.c_str());
            to_linear->setDst(linear_cs_.c_str());
            group->appendTransform(to_linear);
        }

        auto ec = OCIO::ExposureContrastTransform::Create();
        ec->setStyle(OCIO::EXPOSURE_CONTRAST_LINEAR);
        ec->setExposure(exposure_);
        ec->setPivot(0.18);
        ec->makeExposureDynamic();
        group->appendTransform(ec);

        auto dvt = OCIO::DisplayViewTransform::Create();
        dvt->setSrc(linear_cs_.c_str());
        dvt->setDisplay(display_.c_str());
        dvt->setView(view_.c_str());
        group->appendTransform(dvt);

        OCIO::ConstProcessorRcPtr proc = config_->getProcessor(group);
        OCIO::ConstGPUProcessorRcPtr gpu = proc->getDefaultGPUProcessor();
        cpu_ = proc->getDefaultCPUProcessor();

        desc_ = OCIO::GpuShaderDesc::CreateShaderDesc();
        desc_->setLanguage(OCIO::GPU_LANGUAGE_GLSL_4_0);
        desc_->setFunctionName("ocio_display");
        desc_->setResourcePrefix("ocio_");
        gpu->extractGpuShaderInfo(desc_);
    } catch (const std::exception& e) {
        if (error) *error = std::string("OCIO build failed: ") + e.what();
        return false;
    }

    structural_dirty_ = false;
    ++generation_;
    return true;
}

void ColorManager::update_dynamic() {
    if (desc_ && desc_->hasDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE)) {
        auto prop = desc_->getDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE);
        if (auto d = OCIO::DynamicPropertyValue::AsDouble(prop))
            d->setValue(exposure_);
    }
    // The CPU processor carries its own copy of the dynamic state, so the probe
    // would otherwise keep reporting values at exposure 0 while the screen moved.
    if (cpu_ && cpu_->hasDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE)) {
        try {
            auto prop = cpu_->getDynamicProperty(OCIO::DYNAMIC_PROPERTY_EXPOSURE);
            if (auto d = OCIO::DynamicPropertyValue::AsDouble(prop))
                d->setValue(exposure_);
        } catch (const std::exception&) {
            // Non-fatal: the probe falls back to untransformed values.
        }
    }
}

bool ColorManager::apply_display(float rgba[4]) const {
    if (!cpu_) return false;
    try {
        cpu_->applyRGBA(rgba);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

}  // namespace eye

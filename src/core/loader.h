#pragma once

#include <string>
#include <vector>

#include "image.h"

namespace eye {

// Decodes `path` into a float RGBA image. Returns nullptr and fills `error`
// on failure.
//
// Two backends sit behind this one function: OpenImageIO when it is available
// at build time, and a builtin OpenEXR + stb_image path otherwise. Nothing
// above this header knows which one ran -- that is the point of the seam.
ImagePtr load_image(const std::string& path, std::string* error);

// Name of the compiled-in backend, for the About/status line.
const char* loader_backend();

// Extensions the active backend can open, lowercase, without the dot.
std::vector<std::string> supported_extensions();

}  // namespace eye

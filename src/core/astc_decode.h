#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eye {

// A raw ASTC block payload for one 2D image, plus how to read it.
//
// This module is container-agnostic on purpose: today loader_ktx2.cpp feeds it
// from a KTX2 because libktx (<= 4.4.2) has no path for UASTC HDR, but a future
// ".astc" or KTX1 loader can hand it blocks the same way. If a vendored libktx
// gains native ASTC/UASTC-HDR decoding, the KTX call site can drop this while a
// standalone ASTC reader would keep it.
struct AstcImage {
    const uint8_t* data = nullptr;  // tightly packed 16-byte blocks, row-major
    size_t data_len = 0;
    int width = 0;
    int height = 0;
    int block_x = 0;  // 4..12
    int block_y = 0;  // 4..12
    bool hdr = false;   // decode with the HDR profile; results are unbounded
    bool srgb = false;  // LDR only: payload is sRGB-encoded, left un-decoded
};

// Decodes `in` into `out` as width*height*4 float RGBA, row-major, top-down.
// LDR lands in [0,1] with the stored transfer left intact (the loader reports
// electrical values and lets OCIO do the transfer); HDR is linear light.
// Returns false and sets `error` on failure.
bool decode_astc(const AstcImage& in, std::vector<float>& out,
                 std::string* error);

// Whether this build compiled the astcenc-backed decoder in at all.
bool astc_decode_available();

}  // namespace eye

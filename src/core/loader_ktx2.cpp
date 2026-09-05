#include "loader.h"
#include "loader_ktx2.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "core/astc_decode.h"

#ifdef EYEPIECE_WITH_KTX
#include <ktx.h>
#endif

namespace eye {

#ifndef EYEPIECE_WITH_KTX

ImagePtr load_ktx2(const std::string&, std::string* error) {
    if (error)
        *error = "KTX2 support was not compiled in (build with EYEPIECE_WITH_KTX)";
    return nullptr;
}

#else

namespace {

// VkFormat numbers this build can turn into float RGBA without a block decoder.
// Basis payloads (UASTC / ETC1S) are transcoded to R8G8B8A8 before we look at
// this; the rest are the uncompressed layouts a compressor is likely to emit.
enum : uint32_t {
    VK_FORMAT_R8_UNORM = 9,
    VK_FORMAT_R8_SRGB = 15,
    VK_FORMAT_R8G8_UNORM = 16,
    VK_FORMAT_R8G8_SRGB = 22,
    VK_FORMAT_R8G8B8_UNORM = 23,
    VK_FORMAT_R8G8B8_SRGB = 29,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_R8G8B8A8_SRGB = 43,
    VK_FORMAT_R16G16B16A16_SFLOAT = 97,
    VK_FORMAT_R32_SFLOAT = 100,
    VK_FORMAT_R32G32B32_SFLOAT = 106,
    VK_FORMAT_R32G32B32A32_SFLOAT = 109,
};

int bytes_per_pixel(uint32_t vkfmt) {
    switch (vkfmt) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SRGB:
            return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SRGB:
            return 2;
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SRGB:
            return 3;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            return 4;
        case VK_FORMAT_R32_SFLOAT:
            return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            return 8;
        case VK_FORMAT_R32G32B32_SFLOAT:
            return 12;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 0;
    }
}

float half_to_float(uint16_t h) {
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t mant = h & 0x3FFu;
    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;  // +/- zero
        } else {
            exp = 127 - 15 + 1;  // normalize the subnormal
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3FFu;
            out = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1Fu) {
        out = sign | 0x7F800000u | (mant << 13);  // inf / NaN
    } else {
        out = sign | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float f;
    std::memcpy(&f, &out, sizeof(f));
    return f;
}

// Writes npix*4 floats into dst. Values are kept exactly as stored (electrical);
// resolving the transfer function is the color module's job, the same policy the
// stb path in loader.cpp follows for LDR input.
bool decode_level(uint32_t vkfmt, const uint8_t* src, size_t npix, float* dst) {
    auto u8 = [&](size_t i) { return src[i] / 255.0f; };
    switch (vkfmt) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            for (size_t i = 0; i < npix; ++i)
                for (int c = 0; c < 4; ++c) dst[i * 4 + c] = u8(i * 4 + c);
            return true;
        case VK_FORMAT_R8G8B8_UNORM:
        case VK_FORMAT_R8G8B8_SRGB:
            for (size_t i = 0; i < npix; ++i) {
                dst[i * 4 + 0] = u8(i * 3 + 0);
                dst[i * 4 + 1] = u8(i * 3 + 1);
                dst[i * 4 + 2] = u8(i * 3 + 2);
                dst[i * 4 + 3] = 1.0f;
            }
            return true;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R8G8_SRGB:
            for (size_t i = 0; i < npix; ++i) {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = u8(i * 2 + 0);
                dst[i * 4 + 3] = u8(i * 2 + 1);
            }
            return true;
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_SRGB:
            for (size_t i = 0; i < npix; ++i) {
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = u8(i);
                dst[i * 4 + 3] = 1.0f;
            }
            return true;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
            for (size_t i = 0; i < npix * 4; ++i) {
                uint16_t h;
                std::memcpy(&h, src + i * 2, sizeof(h));
                dst[i] = half_to_float(h);
            }
            return true;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            std::memcpy(dst, src, npix * 4 * sizeof(float));
            return true;
        case VK_FORMAT_R32G32B32_SFLOAT:
            for (size_t i = 0; i < npix; ++i) {
                float rgb[3];
                std::memcpy(rgb, src + i * 12, sizeof(rgb));
                dst[i * 4 + 0] = rgb[0];
                dst[i * 4 + 1] = rgb[1];
                dst[i * 4 + 2] = rgb[2];
                dst[i * 4 + 3] = 1.0f;
            }
            return true;
        case VK_FORMAT_R32_SFLOAT:
            for (size_t i = 0; i < npix; ++i) {
                float v;
                std::memcpy(&v, src + i * 4, sizeof(v));
                dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = v;
                dst[i * 4 + 3] = 1.0f;
            }
            return true;
        default:
            return false;
    }
}

void flip_vertical(float* p, int w, int h) {
    const size_t row = static_cast<size_t>(w) * 4;
    std::vector<float> tmp(row);
    for (int y = 0; y < h / 2; ++y) {
        float* a = p + static_cast<size_t>(y) * row;
        float* b = p + static_cast<size_t>(h - 1 - y) * row;
        std::memcpy(tmp.data(), a, row * sizeof(float));
        std::memcpy(a, b, row * sizeof(float));
        std::memcpy(b, tmp.data(), row * sizeof(float));
    }
}

// The 14 ASTC block footprints, in VkFormat order (4x4, 5x4, 5x5, 6x5, ...).
constexpr int kAstcBlocks[14][2] = {{4, 4},   {5, 4},  {5, 5},   {6, 5},
                                    {6, 6},   {8, 5},  {8, 6},   {8, 8},
                                    {10, 5},  {10, 6}, {10, 8},  {10, 10},
                                    {12, 10}, {12, 12}};

// True if `vk` is an ASTC VkFormat, filling in block size and profile. Covers
// VK_FORMAT_ASTC_*_{UNORM,SRGB}_BLOCK (157..184) and the HDR
// VK_FORMAT_ASTC_*_SFLOAT_BLOCK range (1000066000..1000066013) that basisu's
// UASTC HDR mode writes.
//
// PRUNE POINT: this and its call site in load_ktx2() exist only because libktx
// <= 4.4.2 will not hand back pixels for these. Delete both when the vendored
// libktx gains that; astc_decode.* can stay for a standalone .astc reader.
bool astc_from_vkformat(uint32_t vk, int& bx, int& by, bool& hdr, bool& srgb) {
    if (vk >= 157 && vk <= 184) {
        const uint32_t idx = vk - 157;
        bx = kAstcBlocks[idx / 2][0];
        by = kAstcBlocks[idx / 2][1];
        hdr = false;
        srgb = (idx & 1u) != 0;
        return true;
    }
    if (vk >= 1000066000u && vk <= 1000066013u) {
        const uint32_t idx = vk - 1000066000u;
        bx = kAstcBlocks[idx][0];
        by = kAstcBlocks[idx][1];
        hdr = true;
        srgb = false;
        return true;
    }
    return false;
}

// KTX key/value payloads are byte strings. The ones worth showing (KTXwriter,
// KTXorientation, KTXswizzle, ...) are printable ASCII; anything else is noise
// in a metadata table, so summarize it by length.
std::string kv_value_string(const char* v, unsigned int len) {
    unsigned int n = 0;
    while (n < len && v[n] != '\0') ++n;
    bool ascii = n > 0;
    for (unsigned int i = 0; i < n && ascii; ++i) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        ascii = (c == '\t' || c == '\n' || (c >= 0x20 && c <= 0x7E));
    }
    if (ascii) return std::string(v, n);
    return "<" + std::to_string(len) + " bytes>";
}

}  // namespace

ImagePtr load_ktx2(const std::string& path, std::string* error) {
    auto fail = [&](const std::string& m) -> ImagePtr {
        if (error) *error = m;
        return nullptr;
    };

    ktxTexture2* ktx = nullptr;
    KTX_error_code rc = ktxTexture2_CreateFromNamedFile(
        path.c_str(), KTX_TEXTURE_CREATE_LOAD_IMAGE_DATA_BIT, &ktx);
    if (rc != KTX_SUCCESS || !ktx)
        return fail(std::string("libktx: ") + ktxErrorString(rc));

    struct Guard {
        ktxTexture2* t;
        ~Guard() {
            if (t) ktxTexture_Destroy(ktxTexture(t));
        }
    } guard{ktx};

    if (ktx->numDimensions != 2 || ktx->baseDepth > 1 || ktx->isArray ||
        ktx->numFaces != 1) {
        return fail(
            "KTX2: only 2D single-layer textures are handled so far "
            "(this file is 3D, an array, or a cube map)");
    }

    // Read everything that transcoding would overwrite, first.
    const bool needs_transcode = ktxTexture2_NeedsTranscoding(ktx);
    std::string payload;
    if (needs_transcode) {
        payload = (ktx->supercompressionScheme == KTX_SS_BASIS_LZ)
                      ? "ETC1S (BasisLZ)"
                      : "UASTC";
    } else if (ktx->supercompressionScheme == KTX_SS_ZSTD) {
        payload = "zstd-supercompressed";
    } else if (ktx->supercompressionScheme == KTX_SS_ZLIB) {
        payload = "zlib-supercompressed";
    } else {
        payload = "uncompressed";
    }
    const uint32_t src_components = ktxTexture2_GetNumComponents(ktx);
    const khr_df_transfer_e transfer = ktxTexture2_GetOETF_e(ktx);

    if (needs_transcode) {
        rc = ktxTexture2_TranscodeBasis(ktx, KTX_TTF_RGBA32, 0);
        if (rc != KTX_SUCCESS)
            return fail(std::string("KTX2 Basis transcode failed: ") +
                        ktxErrorString(rc));
    }

    const uint32_t vkfmt = ktx->vkFormat;
    const int bpp = bytes_per_pixel(vkfmt);

    // ASTC (incl. basisu UASTC HDR, which libktx <= 4.4.2 cannot decode): pull
    // the raw blocks through astcenc instead of decode_level(). See the
    // PRUNE POINT note on astc_from_vkformat().
    int astc_bx = 0, astc_by = 0;
    bool astc_hdr = false, astc_srgb = false;
    const bool is_astc =
        astc_from_vkformat(vkfmt, astc_bx, astc_by, astc_hdr, astc_srgb);
    if (is_astc) {
        payload = "ASTC " + std::to_string(astc_bx) + "x" +
                  std::to_string(astc_by) + (astc_hdr ? " HDR" : " LDR");
        if (ktx->supercompressionScheme != KTX_SS_NONE)
            return fail("KTX2: ASTC payload is still supercompressed after load "
                        "(scheme " +
                        std::to_string(
                            static_cast<int>(ktx->supercompressionScheme)) +
                        ")");
        if (!astc_decode_available())
            return fail("KTX2: file is " + payload +
                        ", but ASTC decoding was disabled at build time "
                        "(EYEPIECE_WITH_ASTC=OFF)");
    } else if (bpp == 0) {
        return fail("KTX2: vkFormat " + std::to_string(vkfmt) +
                    " has no CPU decoder in this build (block-compressed or an "
                    "uncommon layout)");
    }

    // KTX stores S-right; T is up unless the file says otherwise. eyepiece's
    // buffer is top-down, so flip when the file is y-up.
    const bool flip = (ktx->orientation.y == KTX_ORIENT_Y_UP);
    const uint8_t* data = ktxTexture_GetData(ktxTexture(ktx));
    const uint32_t nlevels = ktx->numLevels ? ktx->numLevels : 1;

    auto img = std::make_shared<Image>();
    img->levels.resize(nlevels);
    for (uint32_t lvl = 0; lvl < nlevels; ++lvl) {
        const int lw =
            static_cast<int>(std::max<uint32_t>(1u, ktx->baseWidth >> lvl));
        const int lh =
            static_cast<int>(std::max<uint32_t>(1u, ktx->baseHeight >> lvl));
        const size_t npix = static_cast<size_t>(lw) * lh;

        ktx_size_t offset = 0;
        rc = ktxTexture_GetImageOffset(ktxTexture(ktx), lvl, 0, 0, &offset);
        if (rc != KTX_SUCCESS)
            return fail(std::string("KTX2: level offset lookup failed: ") +
                        ktxErrorString(rc));

        ImageLevel& out = img->levels[lvl];
        out.width = lw;
        out.height = lh;

        if (is_astc) {
            const ktx_size_t blk_bytes =
                ktxTexture_GetImageSize(ktxTexture(ktx), lvl);
            if (offset + blk_bytes > ktx->dataSize)
                return fail("KTX2: ASTC level " + std::to_string(lvl) +
                            " runs past the loaded data");
            AstcImage ai;
            ai.data = data + offset;
            ai.data_len = blk_bytes;
            ai.width = lw;
            ai.height = lh;
            ai.block_x = astc_bx;
            ai.block_y = astc_by;
            ai.hdr = astc_hdr;
            ai.srgb = astc_srgb;
            std::string derr;
            if (!decode_astc(ai, out.pixels, &derr))
                return fail("KTX2: " + derr);
        } else {
            if (offset + npix * static_cast<size_t>(bpp) > ktx->dataSize)
                return fail("KTX2: level " + std::to_string(lvl) +
                            " runs past the loaded data");
            out.pixels.resize(npix * 4);
            if (!decode_level(vkfmt, data + offset, npix, out.pixels.data()))
                return fail("KTX2: no decoder for vkFormat " +
                            std::to_string(vkfmt));
        }
        if (flip) flip_vertical(out.pixels.data(), lw, lh);
    }
    img->set_active_level(0);

    img->format = "ktx2";
    // astcenc always yields RGBA; basisu's ASTC HDR DFD otherwise reports 1.
    img->source_channels =
        is_astc ? 4 : static_cast<int>(src_components ? src_components : 4);

    if (is_astc) {
        img->hint = astc_hdr    ? ColorHint::SceneLinear
                    : astc_srgb ? ColorHint::sRGB
                                : ColorHint::Unknown;
    } else {
        switch (transfer) {
            case KHR_DF_TRANSFER_SRGB:
                img->hint = ColorHint::sRGB;
                break;
            case KHR_DF_TRANSFER_LINEAR:
                // Linear + a float payload reads as scene-linear; linear 8-bit
                // is usually data (normals, masks) -- let the color module
                // decide.
                img->hint =
                    (bpp >= 8) ? ColorHint::SceneLinear : ColorHint::Unknown;
                break;
            default:
                img->hint = ColorHint::Unknown;
                break;
        }
    }

    auto meta = [&](const std::string& k, const std::string& v) {
        img->metadata.push_back({k, v});
    };
    meta("resolution", std::to_string(ktx->baseWidth) + " x " +
                           std::to_string(ktx->baseHeight));
    meta("channels", std::to_string(img->source_channels));
    meta("ktx2 payload", payload);
    meta("mip levels", std::to_string(nlevels));
    meta("transfer", transfer == KHR_DF_TRANSFER_SRGB     ? "sRGB"
                     : transfer == KHR_DF_TRANSFER_LINEAR ? "linear"
                                                          : "unspecified");
    meta("orientation", flip ? "y-up in file (flipped on load)" : "y-down");
    meta("premultiplied alpha",
         ktxTexture2_GetPremultipliedAlpha(ktx) ? "yes" : "no");
    if (needs_transcode)
        meta("2D view", "transcoded to RGBA8");
    else if (is_astc)
        meta("2D view", astc_hdr ? "decoded via astcenc (linear float)"
                                 : "decoded via astcenc");

    for (ktxHashListEntry* e = ktx->kvDataHead; e; e = ktxHashList_Next(e)) {
        char* key = nullptr;
        unsigned int key_len = 0;
        if (ktxHashListEntry_GetKey(e, &key_len, &key) != KTX_SUCCESS || !key)
            continue;
        char* val = nullptr;
        unsigned int val_len = 0;
        ktxHashListEntry_GetValue(e, &val_len, reinterpret_cast<void**>(&val));
        meta(key, val ? kv_value_string(val, val_len) : std::string("<empty>"));
    }

    img->path = path;
    img->display_name = std::filesystem::path(path).filename().string();
    static const char* kDefault[] = {"R", "G", "B", "A"};
    for (int i = 0; i < std::min(img->source_channels, 4); ++i)
        img->channel_names.push_back(kDefault[i]);

    return img;
}

#endif  // EYEPIECE_WITH_KTX

}  // namespace eye

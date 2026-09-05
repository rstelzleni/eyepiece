#include "core/astc_decode.h"

#ifdef EYEPIECE_WITH_ASTC
#include <astcenc.h>
#endif

namespace eye {

#ifndef EYEPIECE_WITH_ASTC

bool astc_decode_available() { return false; }

bool decode_astc(const AstcImage&, std::vector<float>&, std::string* error) {
    if (error)
        *error = "ASTC decoding was not compiled in (build -DEYEPIECE_WITH_ASTC=ON)";
    return false;
}

#else

bool astc_decode_available() { return true; }

bool decode_astc(const AstcImage& in, std::vector<float>& out,
                 std::string* error) {
    auto fail = [&](const std::string& m) {
        if (error) *error = m;
        return false;
    };
    if (!in.data || in.width <= 0 || in.height <= 0 || in.block_x <= 0 ||
        in.block_y <= 0)
        return fail("ASTC: bad image parameters");

    const astcenc_profile profile =
        in.hdr ? ASTCENC_PRF_HDR
               : (in.srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR);

    // Quality is irrelevant for a decompress-only context but must be a valid
    // preset; a single thread keeps the call self-contained and reentrant.
    astcenc_config config{};
    astcenc_error rc = astcenc_config_init(
        profile, static_cast<unsigned>(in.block_x),
        static_cast<unsigned>(in.block_y), 1, ASTCENC_PRE_FASTEST,
        ASTCENC_FLG_DECOMPRESS_ONLY, &config);
    if (rc != ASTCENC_SUCCESS)
        return fail(std::string("ASTC config: ") + astcenc_get_error_string(rc));

    astcenc_context* ctx = nullptr;
    rc = astcenc_context_alloc(&config, 1, &ctx);
    if (rc != ASTCENC_SUCCESS || !ctx)
        return fail(std::string("ASTC context: ") + astcenc_get_error_string(rc));

    out.assign(static_cast<size_t>(in.width) * in.height * 4, 0.0f);
    void* slice = out.data();
    astcenc_image img{};
    img.dim_x = static_cast<unsigned>(in.width);
    img.dim_y = static_cast<unsigned>(in.height);
    img.dim_z = 1;
    img.data_type = ASTCENC_TYPE_F32;
    img.data = &slice;

    const astcenc_swizzle swz{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B,
                              ASTCENC_SWZ_A};
    rc = astcenc_decompress_image(ctx, in.data, in.data_len, &img, &swz, 0);
    astcenc_context_free(ctx);
    if (rc != ASTCENC_SUCCESS)
        return fail(std::string("ASTC decode: ") + astcenc_get_error_string(rc));
    return true;
}

#endif  // EYEPIECE_WITH_ASTC

}  // namespace eye

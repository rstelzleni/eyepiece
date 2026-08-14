#include "gpu/renderer.h"

#include <epoxy/gl.h>

#include <cmath>
#include <cstdio>

namespace eye {
namespace {

constexpr const char* kVertexShader = R"(#version 410 core
uniform vec4 u_rect;   // x0,y0,x1,y1 in NDC
out vec2 v_uv;
void main() {
    // Quad corners straight from gl_VertexID as a triangle strip -- no buffers.
    vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
    v_uv = c;
    gl_Position = vec4(mix(u_rect.xy, u_rect.zw, c), 0.0, 1.0);
}
)";

// The OCIO-generated source is spliced in ahead of this; it defines
// ocio_display() plus whatever samplers and uniforms that transform needs.
constexpr const char* kFragmentBody = R"(
in vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_image;
uniform vec2  u_image_size;
uniform int   u_channel;       // 0 rgb, 1 r, 2 g, 3 b, 4 a, 5 luma
uniform float u_gamma;
uniform int   u_checker;
uniform float u_checker_size;
uniform int   u_grid;
uniform float u_zoom;

void main() {
    vec4 src = texture(u_image, v_uv);

    // Channel isolation happens in the input colorspace, before the display
    // transform, so an isolated channel is viewed through exactly the same
    // transform as the composite. Alpha is the exception: it is not light, so
    // running a view transform over it would be meaningless.
    bool raw = false;
    vec4 c = src;
    if (u_channel == 1) c = vec4(src.rrr, 1.0);
    else if (u_channel == 2) c = vec4(src.ggg, 1.0);
    else if (u_channel == 3) c = vec4(src.bbb, 1.0);
    else if (u_channel == 4) { c = vec4(src.aaa, 1.0); raw = true; }
    else if (u_channel == 5) {
        float y = dot(src.rgb, vec3(0.2126, 0.7152, 0.0722));
        c = vec4(vec3(y), 1.0);
    } else {
        c = vec4(src.rgb, 1.0);
    }

    vec4 disp = raw ? clamp(c, 0.0, 1.0) : ocio_display(c);

    if (u_gamma != 1.0)
        disp.rgb = pow(max(disp.rgb, vec3(0.0)), vec3(1.0 / u_gamma));

    // Composite over a checker so alpha is visible rather than implied.
    if (u_checker == 1 && u_channel == 0) {
        vec2 t = floor(gl_FragCoord.xy / u_checker_size);
        float k = mod(t.x + t.y, 2.0);
        vec3 bg = mix(vec3(0.28), vec3(0.38), k);
        disp.rgb = mix(bg, disp.rgb, clamp(src.a, 0.0, 1.0));
    }

    // Pixel boundary grid, drawn only once pixels are large enough to have a
    // meaningful interior.
    if (u_grid == 1 && u_zoom >= 6.0) {
        vec2 px = v_uv * u_image_size;
        vec2 f = fract(px);
        vec2 w = fwidth(px) * 1.2;
        float line = max(step(f.x, w.x), step(f.y, w.y));
        float strength = clamp((u_zoom - 6.0) / 12.0, 0.0, 0.35);
        disp.rgb = mix(disp.rgb, vec3(0.0), line * strength);
    }

    frag_color = vec4(disp.rgb, 1.0);
}
)";

unsigned compile(GLenum type, const std::string& src, std::string* error) {
    const unsigned sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len > 0 ? len : 1), '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        if (error) *error = "shader compile failed: " + log;
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

bool Renderer::init(std::string* error) {
    // Core profile requires a bound VAO even when the draw pulls no attributes.
    glGenVertexArrays(1, &vao_);
    if (vao_ == 0) {
        if (error) *error = "failed to create VAO";
        return false;
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    return true;
}

void Renderer::shutdown() {
    clear_image();
    release_ocio_textures();
    if (program_) glDeleteProgram(program_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    program_ = 0;
    vao_ = 0;
    have_program_ = false;
}

void Renderer::clear_image() {
    if (image_tex_) glDeleteTextures(1, &image_tex_);
    image_tex_ = 0;
    image_w_ = image_h_ = 0;
}

void Renderer::set_image(const Image& img) {
    clear_image();
    if (!img.valid()) return;

    glGenTextures(1, &image_tex_);
    glBindTexture(GL_TEXTURE_2D, image_tex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // Full float32 rather than half: the texture is what gets magnified, and
    // rounding the source before display would defeat the point of the probe
    // agreeing with what is on screen.
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, img.width, img.height, 0, GL_RGBA,
                 GL_FLOAT, img.pixels.data());
    // Mip chain exists purely for minification; magnification never touches it.
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    image_w_ = img.width;
    image_h_ = img.height;
}

void Renderer::release_ocio_textures() {
    for (const auto& t : ocio_textures_)
        if (t.id) glDeleteTextures(1, &t.id);
    ocio_textures_.clear();
}

void Renderer::upload_ocio_textures(const OCIO::GpuShaderDescRcPtr& desc) {
    release_ocio_textures();

    for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
        const char* name = nullptr;
        const char* sampler = nullptr;
        unsigned width = 0, height = 0;
        OCIO::GpuShaderDesc::TextureType channel =
            OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderCreator::TextureDimensions dims =
            OCIO::GpuShaderCreator::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
        desc->getTexture(i, name, sampler, width, height, channel, dims, interp);

        const float* values = nullptr;
        desc->getTextureValues(i, values);
        if (!values) continue;

        const bool is_red = (channel == OCIO::GpuShaderDesc::TEXTURE_RED_CHANNEL);
        const GLenum internal = is_red ? GL_R32F : GL_RGB32F;
        const GLenum format = is_red ? GL_RED : GL_RGB;
        const GLenum filter =
            (interp == OCIO::INTERP_NEAREST) ? GL_NEAREST : GL_LINEAR;
        const bool one_d = (dims == OCIO::GpuShaderCreator::TEXTURE_1D);
        const GLenum target = one_d ? GL_TEXTURE_1D : GL_TEXTURE_2D;

        OcioTexture tex;
        tex.target = target;
        tex.sampler = sampler ? sampler : "";
        glGenTextures(1, &tex.id);
        glBindTexture(target, tex.id);
        if (one_d) {
            glTexImage1D(target, 0, internal, width, 0, format, GL_FLOAT, values);
        } else {
            glTexImage2D(target, 0, internal, width, height, 0, format, GL_FLOAT,
                         values);
            glTexParameteri(target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glTexParameteri(target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
        glBindTexture(target, 0);
        ocio_textures_.push_back(tex);
    }

    for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
        const char* name = nullptr;
        const char* sampler = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interp = OCIO::INTERP_LINEAR;
        desc->get3DTexture(i, name, sampler, edge, interp);

        const float* values = nullptr;
        desc->get3DTextureValues(i, values);
        if (!values) continue;

        OcioTexture tex;
        tex.target = GL_TEXTURE_3D;
        tex.sampler = sampler ? sampler : "";
        glGenTextures(1, &tex.id);
        glBindTexture(GL_TEXTURE_3D, tex.id);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F, edge, edge, edge, 0, GL_RGB,
                     GL_FLOAT, values);
        const GLenum filter =
            (interp == OCIO::INTERP_NEAREST) ? GL_NEAREST : GL_LINEAR;
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, filter);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, filter);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_3D, 0);
        ocio_textures_.push_back(tex);
    }
}

void Renderer::bind_ocio_uniforms(const OCIO::GpuShaderDescRcPtr& desc) {
    for (unsigned i = 0; i < desc->getNumUniforms(); ++i) {
        OCIO::GpuShaderDesc::UniformData data;
        const char* name = desc->getUniform(i, data);
        if (!name) continue;
        const GLint loc = glGetUniformLocation(program_, name);
        if (loc < 0) continue;

        switch (data.m_type) {
            case OCIO::UNIFORM_DOUBLE:
                glUniform1f(loc, static_cast<float>(data.m_getDouble()));
                break;
            case OCIO::UNIFORM_BOOL:
                glUniform1i(loc, data.m_getBool() ? 1 : 0);
                break;
            case OCIO::UNIFORM_FLOAT3:
                glUniform3f(loc, data.m_getFloat3()[0], data.m_getFloat3()[1],
                            data.m_getFloat3()[2]);
                break;
            case OCIO::UNIFORM_VECTOR_FLOAT:
                glUniform1fv(loc, static_cast<GLsizei>(data.m_vectorFloat.m_getSize()),
                             data.m_vectorFloat.m_getVector());
                break;
            case OCIO::UNIFORM_VECTOR_INT:
                glUniform1iv(loc, static_cast<GLsizei>(data.m_vectorInt.m_getSize()),
                             data.m_vectorInt.m_getVector());
                break;
            default:
                break;
        }
    }
}

bool Renderer::rebuild_program(ColorManager& color, std::string* error) {
    OCIO::GpuShaderDescRcPtr desc = color.shader_desc();
    if (!desc) {
        if (error) *error = "no OCIO shader description available";
        return false;
    }

    const std::string frag_src = std::string("#version 410 core\n") +
                                 desc->getShaderText() + "\n" + kFragmentBody;

    const unsigned vs = compile(GL_VERTEX_SHADER, kVertexShader, error);
    if (!vs) return false;
    const unsigned fs = compile(GL_FRAGMENT_SHADER, frag_src, error);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }

    const unsigned prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len > 0 ? len : 1), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        if (error) *error = "program link failed: " + log;
        glDeleteProgram(prog);
        return false;
    }

    if (program_) glDeleteProgram(program_);
    program_ = prog;
    upload_ocio_textures(desc);
    color_generation_ = color.generation();
    have_program_ = true;
    return true;
}

bool Renderer::draw(const Viewport& vp, ColorManager& color,
                    const DrawOptions& opts, std::string* error) {
    if (!color.build(error)) return false;

    if (!have_program_ || color_generation_ != color.generation()) {
        if (!rebuild_program(color, error)) return false;
    }
    if (!image_tex_) return true;

    color.update_dynamic();

    const Rect r = vp.image_rect();
    const double w = vp.window_width();
    const double h = vp.window_height();
    // Screen pixels -> NDC. Y flips, so the rect's top edge maps to +1.
    const float x0 = static_cast<float>(2.0 * r.x0 / w - 1.0);
    const float x1 = static_cast<float>(2.0 * r.x1 / w - 1.0);
    const float y0 = static_cast<float>(1.0 - 2.0 * r.y0 / h);
    const float y1 = static_cast<float>(1.0 - 2.0 * r.y1 / h);

    glUseProgram(program_);
    glBindVertexArray(vao_);

    const double zoom = vp.effective_zoom();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image_tex_);
    // The whole point: at or above 1:1 every source pixel becomes an exact
    // square block. Below 1:1, nearest sampling would alias badly, so fall
    // back to the mip chain.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    zoom >= 1.0 ? GL_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
    glUniform1i(glGetUniformLocation(program_, "u_image"), 0);

    int unit = 1;
    for (const auto& t : ocio_textures_) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(t.target, t.id);
        const GLint loc = glGetUniformLocation(program_, t.sampler.c_str());
        if (loc >= 0) glUniform1i(loc, unit);
        ++unit;
    }
    bind_ocio_uniforms(color.shader_desc());

    glUniform4f(glGetUniformLocation(program_, "u_rect"), x0, y0, x1, y1);
    glUniform2f(glGetUniformLocation(program_, "u_image_size"),
                static_cast<float>(image_w_), static_cast<float>(image_h_));
    glUniform1i(glGetUniformLocation(program_, "u_channel"),
                static_cast<int>(opts.channel));
    glUniform1f(glGetUniformLocation(program_, "u_gamma"), opts.gamma);
    glUniform1i(glGetUniformLocation(program_, "u_checker"),
                opts.checkerboard ? 1 : 0);
    glUniform1f(glGetUniformLocation(program_, "u_checker_size"), 16.0f);
    glUniform1i(glGetUniformLocation(program_, "u_grid"), opts.pixel_grid ? 1 : 0);
    glUniform1f(glGetUniformLocation(program_, "u_zoom"),
                static_cast<float>(zoom));

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glBindVertexArray(0);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

}  // namespace eye

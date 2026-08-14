#include <SDL3/SDL.h>
#include <epoxy/gl.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "app/ui.h"
#include "core/loader.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"

namespace {

void usage() {
    std::printf(
        "eyepiece -- a pixel-accurate, color-managed image viewer\n"
        "\n"
        "usage: eyepiece [image ...]\n"
        "\n"
        "mouse:\n"
        "  drag           pan\n"
        "  wheel          zoom by powers of two (exact integer zoom levels)\n"
        "  shift+wheel    fine zoom\n"
        "keys:\n"
        "  f              fit to window        1    zoom 1:1\n"
        "  + / -          zoom step            tab  toggle side panel\n"
        "  c r g b a l    channel view         p    toggle pixel values\n"
        "  left/right     previous/next image  q    quit\n"
        "\n"
        "color management follows $OCIO when set, otherwise the OCIO builtin config.\n"
        "backend: %s\n",
        eye::loader_backend());
}

bool load_into(eye::AppState& state, const std::string& path) {
    std::string error;
    eye::ImagePtr img = eye::load_image(path, &error);
    if (!img) {
        std::fprintf(stderr, "eyepiece: %s\n", error.c_str());
        state.error = error;
        return false;
    }
    img->colorspace = state.color.resolve_hint(img->hint, img->file_colorspace);
    state.session.add(img);
    return true;
}

// Point the viewport, renderer and color input at whatever image is current.
void activate_current(eye::AppState& state, eye::Renderer& renderer,
                      SDL_Window* window, bool refit) {
    const eye::ImagePtr img = state.session.current();
    if (!img) return;

    state.color.set_input_colorspace(img->colorspace);
    state.viewport.set_image(img->width, img->height);
    if (refit) state.viewport.fit();
    renderer.set_image(*img);

    const std::string title = "eyepiece -- " + img->display_name;
    SDL_SetWindowTitle(window, title.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        }
        files.push_back(arg);
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "eyepiece: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

    SDL_Window* window =
        SDL_CreateWindow("eyepiece", 1280, 800,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "eyepiece: window creation failed: %s\n",
                     SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl) {
        std::fprintf(stderr, "eyepiece: GL 4.1 core context failed: %s\n",
                     SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no stray imgui.ini next to images
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 410");

    eye::AppState state;
    std::string error;
    if (!state.color.init(&error)) {
        std::fprintf(stderr, "eyepiece: %s\n", error.c_str());
        return 1;
    }

    eye::Renderer renderer;
    if (!renderer.init(&error)) {
        std::fprintf(stderr, "eyepiece: %s\n", error.c_str());
        return 1;
    }

    for (const auto& f : files) load_into(state, f);

    // Seed the window size before the first fit. Fitting against the default
    // 1x1 viewport would silently produce a nonsense zoom for the first frame.
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        state.viewport.set_window(w, h);
        state.viewport.set_content_inset(
            0.0, 0.0, state.show_panel ? eye::kPanelWidth : 0.0,
            eye::kStatusBarHeightEstimate);
    }
    activate_current(state, renderer, window, /*refit=*/true);

    bool running = true;
    bool dragging = false;

    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            const ImGuiIO& io = ImGui::GetIO();

            switch (event.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    if (event.window.windowID == SDL_GetWindowID(window))
                        running = false;
                    break;

                case SDL_EVENT_DROP_FILE:
                    if (event.drop.data && load_into(state, event.drop.data)) {
                        state.session.set_current(state.session.size() - 1);
                        activate_current(state, renderer, window, true);
                    }
                    break;

                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (!io.WantCaptureMouse &&
                        (event.button.button == SDL_BUTTON_LEFT ||
                         event.button.button == SDL_BUTTON_MIDDLE))
                        dragging = true;
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    dragging = false;
                    break;

                case SDL_EVENT_MOUSE_MOTION:
                    if (dragging)
                        state.viewport.pan(event.motion.xrel, event.motion.yrel);
                    break;

                case SDL_EVENT_MOUSE_WHEEL: {
                    if (io.WantCaptureMouse) break;
                    float mx = 0.0f, my = 0.0f;
                    SDL_GetMouseState(&mx, &my);
                    if (SDL_GetModState() & SDL_KMOD_SHIFT) {
                        // Fine zoom for framing; deliberately not snapped.
                        state.viewport.zoom_at(
                            std::pow(1.15, static_cast<double>(event.wheel.y)),
                            mx, my);
                    } else {
                        state.viewport.zoom_step(
                            static_cast<int>(event.wheel.y > 0    ? 1
                                             : event.wheel.y < 0 ? -1
                                                                 : 0),
                            mx, my);
                    }
                    break;
                }

                case SDL_EVENT_KEY_DOWN: {
                    if (io.WantCaptureKeyboard) break;
                    const double cx = state.viewport.window_width() * 0.5;
                    const double cy = state.viewport.window_height() * 0.5;
                    switch (event.key.key) {
                        case SDLK_ESCAPE:
                        case SDLK_Q: running = false; break;
                        case SDLK_F: state.viewport.fit(); break;
                        case SDLK_1: state.viewport.zoom_1to1(); break;
                        case SDLK_EQUALS:
                        case SDLK_KP_PLUS:
                            state.viewport.zoom_step(1, cx, cy);
                            break;
                        case SDLK_MINUS:
                        case SDLK_KP_MINUS:
                            state.viewport.zoom_step(-1, cx, cy);
                            break;
                        case SDLK_C: state.draw.channel = eye::ChannelView::RGB; break;
                        case SDLK_R: state.draw.channel = eye::ChannelView::Red; break;
                        case SDLK_G: state.draw.channel = eye::ChannelView::Green; break;
                        case SDLK_B: state.draw.channel = eye::ChannelView::Blue; break;
                        case SDLK_A: state.draw.channel = eye::ChannelView::Alpha; break;
                        case SDLK_L: state.draw.channel = eye::ChannelView::Luma; break;
                        case SDLK_P:
                            state.show_pixel_values = !state.show_pixel_values;
                            break;
                        case SDLK_TAB: state.show_panel = !state.show_panel; break;
                        case SDLK_RIGHT:
                        case SDLK_PAGEDOWN:
                            state.session.next();
                            state.image_changed = true;
                            break;
                        case SDLK_LEFT:
                        case SDLK_PAGEUP:
                            state.session.prev();
                            state.image_changed = true;
                            break;
                        default: break;
                    }
                    break;
                }
                default: break;
            }
        }

        int logical_w = 0, logical_h = 0, pixel_w = 0, pixel_h = 0;
        SDL_GetWindowSize(window, &logical_w, &logical_h);
        SDL_GetWindowSizeInPixels(window, &pixel_w, &pixel_h);
        // The viewport works in logical coordinates so its rects line up with
        // ImGui's, which is what keeps the value overlay registered to the
        // pixels it labels. glViewport still needs the real framebuffer size.
        state.viewport.set_window(logical_w, logical_h);

        // Refit only once the viewport knows how big it is this frame.
        if (state.image_changed) {
            activate_current(state, renderer, window, /*refit=*/true);
            state.image_changed = false;
        }

        glViewport(0, 0, pixel_w, pixel_h);
        glClearColor(0.11f, 0.11f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        std::string draw_error;
        if (!renderer.draw(state.viewport, state.color, state.draw, &draw_error)) {
            // Surface it on stderr too, not just in the panel -- a shader that
            // fails to compile is worth seeing without a working window.
            if (draw_error != state.error)
                std::fprintf(stderr, "eyepiece: %s\n", draw_error.c_str());
            state.error = draw_error;
        } else if (!state.error.empty() && state.session.current()) {
            state.error.clear();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        float mouse_x = 0.0f, mouse_y = 0.0f;
        SDL_GetMouseState(&mouse_x, &mouse_y);
        if (!ImGui::GetIO().WantCaptureMouse)
            eye::update_probe(state, mouse_x, mouse_y);

        eye::draw_pixel_value_overlay(state);
        eye::draw_ui(state);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    renderer.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

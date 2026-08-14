# eyepiece

A pixel-accurate, color-managed still-image viewer for Linux. Opens fast from the
command line, shows you the actual numbers in the file, and puts a real OCIO
display transform between those numbers and your screen.

Built for the case where you are hacking on a renderer or a compressor and need
to know what a pixel *is*, not what it looks like after an unlabelled gamma.

## Status

Working scaffold. Loads an image, pans and zooms with exact square pixels,
probes pixel values, applies an OCIO display/view transform on the GPU, and
shows metadata.

## Build

Needs SDL3, OpenColorIO 2.x, libepoxy, and a C++20 compiler. Dear ImGui is
fetched at configure time.

```sh
cmake -S . -B build -G Ninja
cmake --build build
./build/eyepiece image.exr
```

### Decode backend

OpenImageIO is the intended backend — it brings the format coverage and the
metadata that make this tool worth using:

```sh
sudo pacman -S openimageio     # then re-run cmake
```

Without it the build falls back to OpenEXR + stb_image, which covers
exr/png/jpg/hdr/tga and enough EXR header attributes to be useful. Both sit
behind `load_image()` in `src/core/loader.h`; nothing above that seam knows
which one ran. `--help` reports the active backend.

## Controls

| | |
|---|---|
| drag | pan |
| wheel | zoom by powers of two (exact integer zoom levels) |
| shift+wheel | fine zoom |
| `f` / `1` | fit to window / zoom 1:1 |
| `+` `-` | zoom step |
| `c r g b a l` | channel: composite, red, green, blue, alpha, luma |
| `p` / `tab` | toggle pixel values / side panel |
| `←` `→` | previous / next image |
| `q` | quit |

Color management follows `$OCIO` when set, otherwise the OCIO builtin config.

## How it works

```
src/core/     the future libeyepiece -- no SDL, no ImGui, no argv
  image.h       decoded float32 RGBA + metadata
  loader.*      OIIO or builtin, behind one function
  color.*       OCIO config -> GLSL + LUTs, and a CPU path for the probe
  viewport.*    pan/zoom math and the screen<->image mapping
  session.h     the list of loaded images
src/gpu/
  renderer.*    uploads the image, splices OCIO's GLSL into the shader, draws
src/app/        SDL3 window, event loop, ImGui panels
```

Three decisions worth knowing about:

**The float buffer stays on the CPU.** It is not a staging area. The probe reads
it directly, so reported values are what the file contains — not a GPU readback
of whatever the display transform produced. The status bar shows source and
display values side by side.

**OCIO emits the shader.** `getDefaultGPUProcessor()` → `GpuShaderDesc` hands
back GLSL source plus the LUT textures it references, which get spliced into the
fragment shader. Exposure rides an OCIO *dynamic property* rather than being
baked into the transform, so dragging the slider updates a uniform instead of
rebuilding the processor and recompiling the shader every frame.

**Everything agrees via `Viewport::image_rect()`.** That one rect snaps to whole
device pixels at integer zoom, and the renderer, the probe, and the value
overlay all derive from it. Magnification is `GL_NEAREST`; minification falls
back to the mip chain, because nearest sampling below 1:1 aliases badly.

## Deliberate non-goals

Image sequences and playback. That is how this accidentally becomes OpenRV. Not
editing either.

`Session` holds a list rather than one image, because A/B comparison
(wipe/diff) is the one feature already known to be coming, and retrofitting
"there might be two images" through a renderer and UI that assume one is the
rewrite worth twenty lines to avoid.

## Known gaps

- HiDPI: the viewport works in logical coordinates so its rects register with
  ImGui's. On a fractional-scale display, pixel snapping quantizes to logical
  pixels rather than device pixels.
- Large images load fully into RAM. No tiling or paging, so 16k EXRs will hurt.
- Multi-part and deep EXR are not handled; the builtin backend reads the RGBA
  interface only.
- The colorspace dropdown is unfiltered, which is unwieldy against a studio
  config with hundreds of spaces.
- No monitor ICC profile handling, and no Wayland HDR output.

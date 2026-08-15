# eyepiece

A pixel-accurate, color-managed still-image viewer for Linux. Opens fast from the
command line, shows you the actual numbers in the file, and puts a real OCIO
display transform between those numbers and your screen.

Built for the case where you are hacking on a renderer or a compressor and need
to know what a pixel *is*, not what it looks like after an unlabelled gamma.

Usage:

```sh
eyepiece image.png
eyepiece images/*
```

## Status

What exists now is a working scaffold, but it could use a lot more features.
Today it loads an image, pans and zooms with exact square pixels, probes pixel
values, applies an OCIO display/view transform on the GPU, and shows metadata.

I find it useful in this form, but rough.

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
metadata that make this tool worth using. If OIIO is installed `cmake` will
find it and build against it.

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

The source image is decoded into a float buffer on the CPU so that we can
display probe pixel values from the source file without a GPU readback. This
means really large images might use a lot of RAM, but I think this tradeoff is
ok for good performance. The source and display values are shown side by side
in the status bar.

Color conversions happen through an OCIO generated shader on the GPU, when
the backend has OCIO available. This way we get the expected color management
with any LUT textures applied. The exposure slider is applied through a 
uniform in the shader, so it can be adjusted without modifying the generated
shaders.

Image render size is coordinated through `Viewport.image_rect()` which snaps
to whole device pixels at integer zoom, to prevent sampling between pixels.
When zooming in magnification is `GL_NEAREST` so we don't interpolate between
pixels, zooming out uses an image mip chain to avoid aliasing.

## Development direction

Image sequences and playback are out of scope, I'm not trying to rebuild OpenRV.
This is also not intended to become an image editor. It's a viewer only.

Things I'd like to add are, better ui, color copying, image A/B comparison,
more metadata and image analysis options (histograms, intensity, hue, anything
else).

I also built this so that the core is a potentially reusable library you could
embed in another tool, but it remains to be seen how much of it is reusable.

## Known gaps

- HiDPI: the viewport works in logical coordinates so its rects register with
  ImGui's. On a fractional-scale display, pixel snapping quantizes to logical
  pixels rather than device pixels. This "looks good to me" but ymmv.
- Large images load fully into RAM as 4 channel float images. No tiling or
  paging, so 16k EXRs will eat ~2GB of RAM.
- Multi-part and deep EXR are not handled; the builtin backend reads the RGBA
  interface only.
- The colorspace dropdown is unfiltered, which is unwieldy against a studio
  config with hundreds of spaces.
- No monitor ICC profile handling, and no Wayland HDR output.

<!-- SPDX-FileCopyrightText: 2026 Phosphor Works Team -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# plasmazones-shader-render

`plasmazones-shader-render` renders PlasmaZones shaders offscreen, with no compositor, display or screen-capture
tool. It makes shader previews that you can reproduce. The documentation site at
[phosphor-works.org/plasmazones/shaders/](https://phosphor-works.org/plasmazones/shaders/) uses it, and anyone who
needs a stable image of a built-in shader can use it.

## Contents

The tool does these steps:

1. It loads `plasmazones/data/overlays/<id>/metadata.json` and the GLSL files of the shader, the same as the daemon.
   With `--pointer`, it loads a `plasmazones/data/pointer/<id>/` pack instead. Then a synthetic pointer moves
   through the canvas, so the pointer uniform tail has values.
2. It loads `<id>.json` from the layouts in the data of phosphor-engine-libs. Thus the shader draws real zones, not
   one mock rectangle.
3. It starts a Qt Quick scene under `QQuickRenderControl` (offscreen, with no window manager). A `ShaderEffect` fills
   the surface. The `ZoneUniformExtension` of phosphor-rendering is attached, so the `zoneRects[64]` /
   `zoneFillColors[64]` / etc. UBO arrays are the same as at runtime, byte for byte.
4. It renders N frames. For each frame, it advances `iTime` and gives a synthetic audio spectrum to audio-reactive
   shaders.
5. It sends raw RGBA to `ffmpeg` to encode VP9 / H.264, or it saves a PNG sequence directly.

The metadata and layout loaders cover the full schema that the daemon reads.

## Use

```bash
plasmazones-shader-render \
    --shader neon-city \
    --layout master-stack \
    --resolution 1920x1080 \
    --frames 150 \
    --fps 30 \
    --out neon-city.webm
```

| Flag | Default | Purpose |
|------|---------|---------|
| `--shader` (required) | — | id (e.g. `neon-city`) or path to `metadata.json` |
| `--layout` | `master-stack` | id (e.g. `master-stack`) or path to a layout JSON |
| `--resolution` | `1920x1080` | render target — what the shader sees as `iResolution` |
| `--output-size` | same as `--resolution` | downscale the encoded output |
| `--frames` | `150` | total frames |
| `--fps` | `30` | frame rate (drives `iTime` advancement) |
| `--out` | `<id>.webm` | output path; extension picks the format |
| `--audio-mode` | `sine` | one of `silent`, `sine`, `noise`, `sweep` |
| `--shader-dir` | `plasmazones/data/overlays/` (or `data/overlays/` when run from inside `plasmazones/`), then the overlays of the checkout the binary was built from, then the XDG data dirs | where to find shader bundles |
| `--layout-dir` | the same order for `layouts/` | where to find layout JSONs |
| `--still-highlight` | `0` (cycling) | pin one zone, by its zone number, as the only highlighted zone |
| `--pointer` | off | render a pointer pack: `--shader-dir` defaults to `plasmazones/data/pointer/` and a synthetic pointer drives the pointer uniform tail |
| `--pointer-heading` | `0` | direction the synthetic pointer travels, degrees clockwise from screen right |
| `--pointer-speed` | `900` | synthetic pointer speed in logical px per second |
| `--pointer-still` | off | hold the pointer still at the canvas centre instead of sweeping |
| `--press-at` | `0.4` | seconds at which the button goes down (negative: no click) |
| `--release-at` | `0.55` | seconds at which the button comes back up (negative: held for the whole render) |
| `--pointer-button` | `1` | which button the synthetic click uses: 1 left, 2 right, 3 middle |
| `--no-cursor-sprite` | off | leave the cursor sprite unbound so a `needsCursor` pack renders its fallback |
| `--cursor-size` | `24` | drawn size of the synthetic cursor sprite in logical px |

## Build

The tool is optional. Configure with `-DBUILD_TOOLS=ON`:

```bash
cmake -B build -DBUILD_TOOLS=ON
cmake --build build --target plasmazones-shader-render
```

It links `Qt6::Quick`, `PhosphorRendering`, `PhosphorShaders`, `PhosphorWayland`, `PhosphorAudio`,
`PhosphorPointer`, and `PhosphorZoneRendering` of phosphor-libs (the zone entry scaffold that the daemon uses to
assemble packs). For headless CI, it works under software Vulkan
(`VK_ICD_FILENAMES=$(ls /usr/share/vulkan/icd.d/lvp_icd*.json)`). As an alternative, use OpenGL with
`QSG_RHI_BACKEND=opengl`.

## Output formats

The extension of `--out` selects the format:

| Extension | Output |
| --- | --- |
| `.webm` | VP9 via `ffmpeg` (`-c:v libvpx-vp9 -b:v 0 -crf 28 -row-mt 1 -deadline good`) |
| `.mp4` | H.264 via `ffmpeg` (`-c:v libx264 -crf 23`) |
| `.png` | numbered sequence (`out_000001.png`, `out_000002.png`, ...) |

`ffmpeg` must be on `PATH` for the video formats.

## Shared GLSL search order

The tool finds the shared GLSL (`shared/common.glsl`, `audio.glsl`, `zone.vert`) in the source tree first, then in
the XDG data dirs (which include `/usr/share` on a normal system). This order is the opposite of the daemon order,
on purpose:

- The daemon is the installed program, so it uses the installed data first.
- This tool checks the tree that you edit. If an installed copy came first, you would see the last install, not your
  working copy.

The order was once the same as the daemon order. Then a helper added to `shared/common.glsl` but not installed made
every pack that called it fail to compile, though the function was in the file on screen.

## Audio modes

Audio-reactive shaders (each shader with the `audio-reactive` tag in the shader gallery) sample `uAudioSpectrum`.
To give representative output, the renderer supplies a synthetic spectrum:

| Mode | Spectrum | Use |
| --- | --- | --- |
| `silent` | All zeros. Audio shaders show their idle state. | Only to see the dormant geometry. |
| `sine` | Each bar follows a slow phased sine. This is the default. | Looks like steady music at a medium tempo. |
| `noise` | Deterministic noise for each frame. | Looks like a noise bed with a flat spectrum. |
| `sweep` | A peak moves bass → treble → bass in a 4-second loop. | Shows that the low, mid and high buckets light the expected visual elements. |

The bar count is the same as the runtime default (64).

## Render all overlays

The usual site update is a shell loop over the overlays.json list:

```bash
for id in $(jq -r '.[].id' src/data/plasmazones/overlays.json); do
    plasmazones-shader-render \
        --shader "$id" \
        --resolution 960x540 \
        --output-size 480x270 \
        --frames 150 \
        --out public/plasmazones/overlays/$id.webm
done
```

Run it on a developer machine once for each release, and commit the `.webm` files to the docs repository. As an
alternative, run it as a CI step for pull requests that change `plasmazones/data/overlays/`.

## Shared code with the daemon

The tool assembles a pack that defines `pZone` or `pImage` with the same entry-point scaffold as the daemon. If the
assembly is different, the preview renders a different program than the runtime. That is the failure that this tool
must prevent. The scaffold (`zoneentryscaffold.h`) comes from the linked `PhosphorZoneRendering` target.

`ZoneUniformExtension` and the `ZoneShaderUniforms` UBO struct are in
`phosphor-libs/libs/phosphor-rendering/`. They come through the linked `PhosphorRendering::PhosphorRendering` target,
the same as other library headers.

## Limits

- **Display scale:** the tool gives the device-pixel ratio of the render target to `ZoneUniformExtension` in the
  same way as the daemon item. Here the ratio is always 1.0, so a corner radius or border width has the units that
  you asked for. A larger `--resolution` is NOT the same as a scaled display. The zone rectangles grow with
  `iResolution`, but the radius stays the same, so the corners become relatively smaller. A real scaled display
  also scales the radius, so the corners keep their relative size. The tool cannot preview a scale other than 1.0.
- **Multipass and buffer-feedback shaders** (neon-city, voxel-terrain, paper, ...): the frame loop calls
  polishItems / `beginFrame()` / sync / `render()`. The matching `endFrame()` is in `captureFrame` after the
  readback, so the ping-pong order between the buffer textures stays explicit. `buildOffscreenTarget` makes the
  offscreen targets, and `Renderer::render` has the loop. Compare the output with a real daemon render on the first
  run.
- **Depth-buffered shaders:** the tool forwards the `useDepthBuffer = true` flag, and `ShaderNodeRhi` sets up the
  offscreen depth attachment. Compare the output with a real daemon render before you trust CI output.
- **Layout zone colors:** the loader takes the fill and border colors of each zone from the layout JSON. If the JSON
  has no colors, the loader cycles through the Phosphor brand palette. Some layout JSONs have no color overrides. If
  the preview of a shader looks flat, check the layout file.

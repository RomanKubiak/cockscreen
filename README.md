# Cockscreen

A Qt6 / OpenGL ES shader pipeline for Raspberry Pi Zero 2 W and x86_64 Linux. It captures video, audio, MIDI, and OSC data and drives a real-time GLSL shader chain. Typical uses: live VJing, AV installations, generative visuals.

- USB video grabber capture (V4L2)
- Shader-based video effects (OpenGL ES 2 / desktop GL 2.1)
- Audio-driven modulation (ALSA loopback / WASAPI on Windows)
- OSC control input (UDP)
- MIDI control input (ALSA sequencer / WinMM on Windows)
- Windows cross-compilation via MinGW-w64
- Analog CV input and precision CV output paths for modular-style control voltage work

## Remote target

```text
ssh://atom@192.168.41.190
```

Local x86_64 scenes default to a 1024×600 Qt6 window. Pi Zero 2 W builds use `eglfs` directly on DRM/KMS without a compositor.

---

## Startup

Runtime startup is scene-driven. Launch with `--scene-file <path>` or place the platform scene in `scenes/` next to the executable so `cockscreen` can auto-detect it.

Scene files can be plain JSON or JSONC. JSONC is recommended when you want inline `//` or `/* ... */` comments in presets. Default scene auto-detection prefers `.scene.jsonc` and falls back to `.scene.json`.

The only supported CLI options are:

- `--help`
- `--list-devices`
- `--scene-file <path>`
- `--enable-web-server <url>`

### Web control

The Qt shader runtime can expose a small live-control server for scene tweaks while the app is running.

Example:

```bash
./out/build/local-x86_64-debug/cockscreen \
    --scene-file scenes/x86_64-linux.scene.jsonc \
    --enable-web-server http://0.0.0.0:8080
```

Supported bind hosts are `localhost`, `0.0.0.0`, or a numeric IP address. The server stays disabled unless `--enable-web-server` is provided.

Available endpoints:

- `/` mobile-friendly control page for background and shader-chain edits
- `/api/state` current scene/device state as JSON
- `/api/apply` live scene updates via JSON `POST`

The current web UI supports:

- browsing scene presets from the scene directory tree, where directories map to groups and files map to scenes
- enabling or disabling the `video`, `playback`, and `screen` layers
- editing each layer's ordered shader chain
- editing playback transport values for the playback input
- toggling the film-style playback timecode overlay
- tuning `pink_key.glsl` audio detector mode and audio/MIDI reactivity
- background colour and background image selection
- viewing opened and available devices

Device reopening is still read-only in this first version.

Preset discovery is directory-based. Starting from the active scene root, each directory becomes a preset group and each `.scene.jsonc` or `.scene.json` file becomes a selectable scene in the web UI.

Treat the initial startup scene as read-only, whether it came from `--scene-file` or default auto-detection. This allows multiple windows to run from the same source scene in parallel without fighting over in-place edits. Future preset-writing flows must not rewrite that source file in place; they should create a new preset file instead.

---

## Scene file

A scene JSON or JSONC file controls every visual aspect of a run, including the render backend, inputs, mappings, and final compositing.

This section documents the current parser behavior in [src/runtime/scene/Parse.cpp](/home/atom/devel/cockscreen/src/runtime/scene/Parse.cpp) and the runtime defaults in [include/cockscreen/runtime/Scene.hpp](/home/atom/devel/cockscreen/include/cockscreen/runtime/Scene.hpp). Where the parser accepts aliases or applies clamping, that is called out explicitly.

### Top-level fields

| Field | Type | Description |
|---|---|---|
| `render_path` | string | Render backend. Current runtime choices are `qt`, `qt-shader`, and `v4l2-dmabuf-egl`. Parser default: `qt-shader`. |
| `geometry` | object | Window size object. Parser default: `{ "width": 1024, "height": 600 }`. |
| `width` / `height` | integer | Legacy top-level geometry fallback used only when `geometry` is omitted. Values are clamped to minimum `1`. |
| `resources_directory` | string | Path resolved relative to the scene file directory. Parser default: scene file directory itself. |
| `shader_directory` | string | Path resolved relative to the scene file directory unless already absolute. |
| `note_font_file` | string | Font file path, usually relative to `resources_directory`. Optional. |
| `show_status_overlay` | bool | Diagnostic HUD toggle. Runtime default: `true`. |
| `timecode` | bool | Analog-film-style playback timecode toggle. Runtime default: `false`. |
| `pink_key` | object | Optional defaults for `pink_key.glsl` audio/MIDI reactivity. See below. |
| `background_color` | object | Top-level clear colour. Used directly, or as fallback when no nested `inputs.background_color` or `inputs.background` is provided. |
| `background` | object | Accepted alias for `background_color` at top level and inside `inputs`. |
| `background_image` | object or string | Background image configuration. Object form is preferred. String form is accepted as a direct file path. |
| `background_image_file` | string | Legacy fallback used only when `background_image.file` is absent. |
| `background_image_placement` | string | Legacy fallback used only when `background_image.placement` is absent. |
| `inputs` | object | Container for `video`, `playback`, `audio`, `midi`, and optional nested `background_color` or `background`. |
| `video` | object | Shader layer configuration for the video layer. |
| `playback` | object | Shader layer configuration for the playback layer. |
| `screen` | object | Shader layer configuration for the screen layer. |
| `layer_order` | array | Optional explicit compositing order for `screen`, `video`, and `playback`, listed from back to front. |
| `midi_cc_mappings` | array | MIDI CC to shader uniform mappings. |
| `midi_note_mappings` | array | MIDI note to shader uniform mappings. |
| `osc_mappings` | array | OSC address to shader uniform mappings. |

### Parser defaults and clamping

| Value | Parser behavior |
|---|---|
| `geometry.width`, `geometry.height`, top-level `width`, `height` | Clamped to minimum `1`. |
| colour channels `r/g/b/a` and aliases `red/green/blue/alpha` | Clamped to `[0, 1]`. |
| `inputs.*.scale` | Clamped to minimum `0.01`. Default `1.0`. |
| `start_ms`, `loop_start_ms`, `loop_end_ms` | Clamped to minimum `0`. |
| `loop_repeat` | Clamped to minimum `0`. |
| `playback_rate`, `playback_rate_looping` | Clamped to minimum `0.01`. |
| `pink_key.audio_algorithm` | Clamped to `[0, 5]`. |
| `pink_key.audio_reactivity`, `pink_key.midi_reactivity` | Clamped to `[0, 1.5]`. |
| `midi_*_mappings[*].exponent`, `osc_mappings[*].exponent` | Clamped to minimum `0.01`. |

### Multiple-choice fields

| Field | Accepted values | Notes |
|---|---|---|
| `render_path` | `qt`, `qt-shader`, `v4l2-dmabuf-egl` | Runtime-valid values. Parser accepts any string and leaves runtime validation to startup. |
| `background_image.placement` | `center`, `stretched`, `proportional-stretch`, `tiled` | Parser also accepts aliases `centered`, `propotional-stretch`, and `proportional_stretch`. Unknown values fall back to `center`. |
| `layer_order[*]` | `video`, `playback`, `screen` | Entries are case-insensitive. Invalid or duplicate entries are ignored. The whole field is accepted only if all three unique layer names are present. |
| `pink_key.audio_algorithm` | `0`, `1`, `2`, `3`, `4`, `5` | `0` bass focus, `1` low-mid, `2` high-mid, `3` high, `4` spectral centroid, `5` full-spectrum average. |

### `geometry`

```json
"geometry": {
    "width": 1024,
    "height": 600
}
```

### `background_image`

```json
"background_image": {
    "file": "textures/my_bg.jpg",
    "placement": "proportional-stretch"
}
```

`placement` values: `center`, `stretched`, `proportional-stretch`, `tiled`.

Accepted parser aliases for `placement`:

- `centered` -> `center`
- `propotional-stretch` -> `proportional-stretch`
- `proportional_stretch` -> `proportional-stretch`

### `pink_key`

```json
"pink_key": {
    "audio_algorithm": 0,
    "audio_reactivity": 0.45,
    "midi_reactivity": 0.35
}
```

`audio_algorithm` values:

- `0`: bass focus using FFT bands `0..3`
- `1`: low-mid focus using FFT bands `4..7`
- `2`: high-mid focus using FFT bands `8..11`
- `3`: high focus using FFT bands `12..15`
- `4`: weighted spectral centroid across all 16 FFT bands
- `5`: full-spectrum average energy

### `inputs`

```json
"inputs": {
    "video": {
        "enabled": true,
        "device": "/dev/video1",
        "format": "qvga",           // "qvga" (320×240), "vga", "hd" etc.
        "scale": 0.5,               // display scale relative to window
        "on_top": false,            // legacy fallback when layer_order is omitted
        "position": { "x": 0.5, "y": 0.5 }
    },
    "playback": {
        "enabled": true,
        "file": "videos/clip.mp4",  // relative to resources_directory
        "scale": 0.28,
        "on_top": true,
        "position": { "x": 0.02, "y": 0.02 },
        "start_ms": 0,
        "loop_start_ms": 0,
        "loop_end_ms": 8000,
        "loop_repeat": 0,
        "playback_rate": 1.0,
        "playback_rate_looping": 0.5
    },
    "background_color": { "r": 0, "g": 0, "b": 0, "a": 1 },
    "audio": {
        "enabled": true,
        "device": "PCM2902 Audio Codec"  // ALSA source name substring
    },
    "midi": {
        "enabled": true,
        "device": "through"   // ALSA port name substring or numeric port
    }
}
```

Recognized `inputs` children:

- `video`
- `playback`
- `audio`
- `midi`
- `background_color`
- `background`

Parser defaults for every `inputs.*` object use the same `SceneInput` defaults:

| Field | Type | Default | Notes |
|---|---|---|---|
| `enabled` | bool | `true` | If `false`, parser clears `device` and `format` after parsing. |
| `device` | string | `""` | Used by `video`, `audio`, and `midi`. |
| `file` | string | `""` | Used by `playback`. |
| `format` | string | `""` | Used mainly by `video`. |
| `scale` | float | `1.0` | Clamped to minimum `0.01`. |
| `on_top` | bool | omitted | Optional legacy fallback when `layer_order` is omitted. |
| `position` | object | `{ "x": 0, "y": 0 }` | Preferred form. |
| `position_x`, `position_y` | float | `0.0` | Legacy fallback when `position` object is absent. |
| `start_ms` | integer | `0` | Parsed for all inputs, but meaningful for playback. |
| `loop_start_ms` | integer | `0` | Parsed for all inputs, but meaningful for playback. |
| `loop_end_ms` | integer or `null` | omitted | Omitted or `null` disables custom loop end. |
| `loop_repeat` | integer | `0` | `0` means infinite looping in playback logic. |
| `playback_rate` | float | `1.0` | Clamped to minimum `0.01`. |
| `playback_rate_looping` | float | `1.0` | Clamped to minimum `0.01`. |

Playback transport fields on `inputs.playback`:

- `start_ms`: initial playback position in milliseconds
- `loop_start_ms`: loop segment start in milliseconds
- `loop_end_ms`: loop segment end in milliseconds; omit it to disable custom looping
- `loop_repeat`: number of extra loop passes; `0` means infinite looping
- `playback_rate`: playback speed used outside the loop segment
- `playback_rate_looping`: playback speed used while the player is inside the active loop segment

### Shader layers

Three composited layers: `video`, `playback`, and `screen`. Each has an ordered list of GLSL shaders applied as a chain — the output of one becomes `u_texture` for the next.

```json
"video":    { "enabled": true,  "shaders": ["pink_key.glsl", "video_sphere.glsl"] },
"playback": { "enabled": false, "shaders": ["pixelize_loop.glsl"] },
"screen":   { "enabled": true,  "shaders": ["wireframe_sphere.glsl"] }
```

Use `layer_order` to explicitly control the final screen compositing order. The array is interpreted from back to front, so the last item is drawn on top.

```json
"layer_order": ["screen", "video", "playback"]
```

`layer_order` must contain `screen`, `video`, and `playback` exactly once. If it is omitted, the runtime falls back to the older `inputs.video.on_top` behavior.

Each layer object accepts only these fields during parsing:

| Field | Type | Default | Notes |
|---|---|---|---|
| `enabled` | bool | `true` | |
| `shaders` | array of strings | `[]` | Non-string entries are ignored. |

### `midi_cc_mappings`

Map a MIDI CC value to any shader uniform:

```json
"midi_cc_mappings": [
    {
        "layer":    "screen",
        "shader":   "wireframe_sphere.glsl",   // filename only, partial match ok
        "uniform":  "u_wire_density",
        "channel":  0,
        "cc":       91,
        "min":      0.0,
        "max":      1.0,
        "exponent": 1.0   // > 1 = logarithmic feel
    }
]
```

Parser fields for each MIDI CC mapping:

| Field | Type | Default | Notes |
|---|---|---|---|
| `layer` | string | `""` | Required for the mapping to be kept. |
| `shader` | string | `""` | Optional filename or partial shader path match. |
| `uniform` | string | `""` | Required for the mapping to be kept. |
| `channel` | integer | `-1` | |
| `cc` | integer | `0` | Stored internally as controller number. |
| `min` | float | `0.0` | |
| `max` | float | `1.0` | |
| `exponent` | float | `1.0` | Clamped to minimum `0.01`. |

### `midi_note_mappings`

Map MIDI note activity to a shader uniform:

```json
"midi_note_mappings": [
    {
        "layer":   "screen",
        "shader":  "crt_noise.glsl",
        "uniform": "u_note_flash",
        "channel": 0,
        "note":    60,
        "min":     0.0,
        "max":     1.0,
        "exponent": 1.0
    }
]
```

Parser fields for each MIDI note mapping:

| Field | Type | Default | Notes |
|---|---|---|---|
| `layer` | string | `""` | Required for the mapping to be kept. |
| `shader` | string | `""` | Optional filename or partial shader path match. |
| `uniform` | string | `""` | Required for the mapping to be kept. |
| `channel` | integer | `-1` | |
| `note` | integer | `-1` | |
| `min` | float | `0.0` | |
| `max` | float | `1.0` | |
| `exponent` | float | `1.0` | Clamped to minimum `0.01`. |

`decay` is not currently parsed from scene files.

### `osc_mappings`

```json
"osc_mappings": [
    {
        "address":  "/cockscreen/key/hue",
        "layer":    "video",
        "shader":   "pink_key.glsl",
        "uniform":  "u_key_h",
        "min":      0.0,
        "max":      1.0,
        "exponent": 1.0
    }
]
```

Parser fields for each OSC mapping:

| Field | Type | Default | Notes |
|---|---|---|---|
| `address` | string | `""` | Required for the mapping to be kept. |
| `layer` | string | `""` | Required for the mapping to be kept. |
| `shader` | string | `""` | Optional filename or partial shader path match. |
| `uniform` | string | `""` | Required for the mapping to be kept. |
| `min` | float | `0.0` | |
| `max` | float | `1.0` | |
| `exponent` | float | `1.0` | Clamped to minimum `0.01`. |

The OSC server currently listens on `0.0.0.0:9000`. Values arriving on a mapped address are clamped to `[0, 1]`, exponent-mapped, then scaled to `[min, max]` before being set as the uniform.

### Fields currently present in shipped scene presets

Across [scenes/x86_64-linux.scene.jsonc](/home/atom/devel/cockscreen/scenes/x86_64-linux.scene.jsonc), [scenes/pizero-linux.scene.jsonc](/home/atom/devel/cockscreen/scenes/pizero-linux.scene.jsonc), and [scenes/windows.scene.jsonc](/home/atom/devel/cockscreen/scenes/windows.scene.jsonc), the currently used scene-file fields are:

- `resources_directory`
- `note_font_file`
- `shader_directory`
- `render_path`
- `geometry.width`
- `geometry.height`
- `inputs.video.enabled`
- `inputs.video.device`
- `inputs.video.format`
- `inputs.video.scale`
- `inputs.video.on_top`
- `inputs.video.position.x`
- `inputs.video.position.y`
- `inputs.playback.enabled`
- `inputs.playback.file`
- `inputs.playback.scale`
- `inputs.playback.on_top`
- `inputs.playback.position.x`
- `inputs.playback.position.y`
- `inputs.playback.start_ms`
- `inputs.playback.loop_start_ms`
- `inputs.playback.loop_end_ms`
- `inputs.playback.loop_repeat`
- `inputs.playback.playback_rate`
- `inputs.playback.playback_rate_looping`
- `inputs.background_color.r`
- `inputs.background_color.g`
- `inputs.background_color.b`
- `inputs.background_color.a`
- `inputs.audio.enabled`
- `inputs.audio.device`
- `inputs.midi.enabled`
- `inputs.midi.device`
- `background_image.file`
- `background_image.placement`
- `show_status_overlay`
- `timecode`
- `pink_key.audio_algorithm`
- `pink_key.audio_reactivity`
- `pink_key.midi_reactivity`
- `video.enabled`
- `video.shaders[]`
- `playback.enabled`
- `playback.shaders[]`
- `screen.enabled`
- `screen.shaders[]`
- `layer_order[]`
- `midi_cc_mappings[].layer`
- `midi_cc_mappings[].shader`
- `midi_cc_mappings[].uniform`
- `midi_cc_mappings[].channel`
- `midi_cc_mappings[].cc`
- `midi_cc_mappings[].min`
- `midi_cc_mappings[].max`
- `midi_cc_mappings[].exponent`
- `osc_mappings[].address`
- `osc_mappings[].layer`
- `osc_mappings[].shader`
- `osc_mappings[].uniform`
- `osc_mappings[].min`
- `osc_mappings[].max`
- `osc_mappings[].exponent`

### Analog front end

The Pi AARCH64 analog wiring, mux pinout, gate inputs, power distribution, and precision CV output stage now live in [docs/ANALOG.md](docs/ANALOG.md).
The split schematics are [docs/cv-power-input-stage.svg](docs/cv-power-input-stage.svg) and [docs/cv-output-stage.svg](docs/cv-output-stage.svg).
The matching hardware bill of materials is in [docs/BOM.md](docs/BOM.md).

---

## Global shader uniforms

Every shader receives these automatically — no mapping needed:

| Uniform | Type | Description |
|---|---|---|
| `u_texture` | `sampler2D` | Input texture from the previous stage (or blank if first). |
| `u_time` | `float` | Seconds since the window opened. |
| `u_viewport_size` | `vec2` | Window dimensions in pixels. |
| `u_video_size` | `vec2` | Dimensions of the source video frame. |
| `u_audio_level` | `float` | Overall audio level in dB (normalised 0–1). |
| `u_audio_rms` | `float` | RMS audio level 0–1. |
| `u_audio_peak` | `float` | Peak audio level 0–1. |
| `u_audio_fft[16]` | `float[]` | 16-band FFT magnitude 0–1 (low→high frequency). |
| `u_audio_waveform[64]` | `float[]` | 64-sample waveform buffer −1..1. |
| `u_midi_primary` | `float` | Most-recent MIDI note number normalised 0–1. |
| `u_midi_secondary` | `float` | Second most-recent MIDI note 0–1. |
| `u_midi_notes[8]` | `float[]` | Last 8 held note numbers 0–1. |
| `u_midi_velocities[8]` | `float[]` | Velocities for those notes 0–1. |
| `u_midi_ages[8]` | `float[]` | Age (seconds) of each held note. |
| `u_midi_channels[8]` | `float[]` | MIDI channel 0–15 for each held note. |
| `u_note_label_atlas` | `sampler2D` | 16×8 glyph atlas (128 MIDI note labels) rendered from `note_font_file`. Bound to texture unit 1. |
| `u_note_label_grid` | `vec2` | Atlas grid size `(16, 8)`. |
| `u_icon_atlas` | `sampler2D` | 8×8 icon atlas from `Font Awesome 7 Free-Solid-900.otf`. Bound to texture unit 2. Available when the font file is present in `resources/fonts/`. |
| `u_icon_grid` | `vec2` | Icon atlas grid size `(8, 8)`. |

---

## Shaders

Single-pass ShaderToy image shaders are also supported through a compatibility wrapper when the shader defines `mainImage(...)` and does not define its own `main()`. The runtime injects `iTime`, `iTimeDelta`, `iFrame`, `iFrameRate`, `iResolution`, `iMouse`, `iDate`, `iChannel0..3`, `iChannelResolution`, `iChannelTime`, and `iSampleRate`.

Current limits: only the `Image` pass shape is emulated. `iChannel0` is the current stage input texture, `iChannel1..3` are blank textures, and ShaderToy multipass buffers / cubemaps / keyboard / sound inputs are not implemented.

### `passthrough.glsl`
Copies `u_texture` unchanged. Useful as a no-op placeholder in a chain.

### `basic.glsl`
Outputs a solid colour. No significant uniforms.

### `audio_waveform.glsl`
Draws the live audio waveform as a line over the background.

**Audio uniforms used:** `u_audio_waveform[64]`, `u_time`

### `crt_noise.glsl`
CRT monitor simulation: scanlines, barrel distortion, RGB fringing, phosphor flicker, and composite noise.

**Custom uniforms (MIDI/OSC mappable):**

| Uniform | Range | Default | Description |
|---|---|---|---|
| `u_note_flash` | 0–1 | 0 | Brightens the whole image — map to a MIDI note for beat flashes. |
| `u_note_glitch` | 0–1 | 0 | Adds horizontal glitch / scanline shift — map to note-on for glitch hits. |

**Audio uniforms used:** `u_audio_level`, `u_time`

### `edge_distortion.glsl`
Warps the image near frame edges. Self-contained, no extra uniforms.

**Audio uniforms used:** `u_time`

### `horro_ink.glsl`
Ink-bleed edge detection with rapid temporal distortion: high-frequency shaking, claw distortion, colour posterisation cycling red/purple, animated vignette. Very fast visual chaos.

**Audio uniforms used:** `u_time`

### `ir_boost.glsl`
Simulates infrared camera look: desaturation, green/contrast boost, hot highlights.

No extra uniforms.

### `kaleidoscope.glsl`
Mirrors and rotates the input into radial symmetry.

**Audio uniforms used:** `u_time`, `u_audio_level`

### `line_sync_error.glsl`
Horizontal line-tear / sync-loss artefact.

**Audio uniforms used:** `u_time`

### `pink_key.glsl`
Chroma key / colour isolator. Cuts the selected hue and replaces it with transparency.

**Custom uniforms (MIDI/OSC mappable):**

| Uniform | Range | Default | Description |
|---|---|---|---|
| `u_key_h` | 0–1 | 0.9 | Target hue to key out (0 = red, 0.16 = yellow, 0.33 = green, 0.66 = blue). |
| `u_key_hrange` | 0–0.5 | 0.08 | Half-width of the hue acceptance window (±29° at default). |
| `u_key_spread` | 0–1 | 0.5 | Edge softness — higher = more gradual transition. |
| `u_key_smin` | 0–1 | 0.15 | Minimum pixel saturation to key; prevents keying near-grey pixels. |

### `pixel_ruler.glsl`
Draws a pixel grid ruler overlay useful for alignment.

**Audio uniforms used:** `u_viewport_size`

### `pixelize_loop.glsl`
Animates a continuous pixelisation / mosaic effect that pulses with audio.

**Audio uniforms used:** `u_time`, `u_audio_level`

### `pixelize_viewport.glsl`
Static or animated pixelisation with an externally controlled amount.

**Custom uniforms (MIDI/OSC mappable):**

| Uniform | Range | Default | Description |
|---|---|---|---|
| `u_pixelize_amount` | 0–1 | 0 | 0 = full resolution, 1 = maximum block size. |

### `rorschach_inkblot.glsl`
Generates animated Rorschach-style bilateral ink symmetry using noise.

**Audio uniforms used:** `u_time`, `u_audio_level`

### `triangle_rotate.glsl`
Spinning geometric triangle pattern.

**Audio uniforms used:** `u_time`, `u_audio_level`

### `video_feedback.glsl`
Simulates camera-into-monitor video feedback tunnelling.

**Audio uniforms used:** `u_time`

### `video_sphere.glsl`
Maps the source video texture onto a 3D sphere with specular lighting.

**Custom uniforms (MIDI/OSC mappable):**

| Uniform | Range | Default | Description |
|---|---|---|---|
| `u_sphere_radius` | 0–0.5 | 0.14 | Radius of the sphere relative to viewport height. |
| `u_specular` | 0–1 | 0.5 | Specular highlight intensity. |
| `u_spin_speed` | 0–2 | 0.15 | Rotation speed in revolutions per second. |
| `u_tilt` | 0–1 | 0.18 | Axis tilt angle. |

**Audio uniforms used:** `u_audio_level` (pulse radius on beat)

### `spectrum_sphere.glsl`
Audio-reactive sphere with FFT-coloured wireframe bands. Each latitude band is coloured by its corresponding FFT bin — the geometry pulses with the spectrum.

**Audio uniforms used:** `u_audio_fft[16]`, `u_audio_level`, `u_time`

**Custom uniforms (MIDI/OSC mappable):**

| Uniform | Range | Default | Description |
|---|---|---|---|
| `u_wire_density` | 0–1 | 0.5 | Density of wireframe lines per band. |

### `wireframe_sphere.glsl`
White wireframe sphere that bounces off the viewport edges and spins. Blends over the background.

**Custom uniforms (MIDI/OSC mappable):**

| Uniform | Range | Default | Description |
|---|---|---|---|
| `u_wire_density` | 0–1 | 0.5 | Line density / number of parallels and meridians. |

**Audio uniforms used:** `u_audio_level` (modulates radius), `u_audio_fft[16]`, `u_time`

### `midi_dots.glsl`
Renders held MIDI notes as animated dots. Each of the 8 tracked notes spawns a coloured ring whose size encodes velocity.

**Audio/MIDI uniforms used:** `u_midi_notes[8]`, `u_midi_velocities[8]`, `u_midi_ages[8]`, `u_time`

### `font_awesome_burst.glsl`
Renders a 3×5 grid of random Font Awesome icons centered in the viewport. Every 100 ms a new random icon is selected per slot. Icons are white, background shows through.

Requires `Font Awesome 7 Free-Solid-900.otf` in `resources/fonts/`. The icon atlas (64 codepoints, 8×8 grid) is built automatically at startup.

**Uniforms used:** `u_icon_atlas`, `u_icon_grid`, `u_time`

No MIDI/audio mappable uniforms — the change rate can be adjusted by editing `kChangeHz` in the shader (default 10 per second).

### `toy_shader.glsl`
First pass of a native two-stage port of a vaporwave-style logo effect. It shifts the input image horizontally through a scanning band and stamps a procedural monochrome logo mask into the frame.

**Uniforms used:** `u_texture`, `u_time`

### `toy_shader_image.glsl`
Second pass for the vaporwave port. It recombines colour channels, restores the image through a matrix transform, and adds scanline/static treatment.

**Uniforms used:** `u_texture`, `u_time`, `u_resolution`

### `toy_synthwave.glsl`
Single-pass ShaderToy-style synthwave horizon and grid effect. This shader uses the ShaderToy compatibility wrapper, so it keeps its `mainImage(...)` entrypoint and reads the previous stage through `iChannel0`.

**ShaderToy uniforms used:** `iTime`, `iResolution`, `iChannel0`, `iChannelResolution[0]`

---

## Build

### Local x86_64 Linux (debug)

```bash
cmake --preset local-x86_64-debug
cmake --build --preset local-x86_64-debug
./out/build/local-x86_64-debug/cockscreen --scene-file scenes/x86_64-linux.scene.json
```

### Pi Zero 2 W native build (via SSH)

```bash
cmake --preset pi-zero2w-debug
cmake --build --preset pi-zero2w-debug
```

### Pi Zero 2 W cross-compilation

```bash
./scripts/bootstrap-pizero-cross.sh
cmake --preset cross-pi-zero2w-debug
cmake --build --preset cross-pi-zero2w-debug
```

### Windows (MinGW cross-compilation from Linux)

```bash
cmake --preset windows-x86_64-release
cmake --build --preset windows-x86_64-release
# Produces out/build/windows-x86_64-release/deploy/ — self-contained .exe + DLLs
```

---

## VS Code tasks

| Task | Description |
|---|---|
| `Local: Build Debug` | CMake configure + build for x86_64 Linux |
| `Local: Run Debug` | Build then run locally |
| `Local: Cross Build Debug` | Cross-compile for Pi (debug) |
| `Local: Cross Build Release` | Cross-compile for Pi (release) |
| `Remote Pi: Sync Workspace` | rsync source to Pi |
| `Remote Pi: Build Debug` | Sync + native build on Pi (debug) |
| `Remote Pi: Build Release` | Sync + native build on Pi (release) |
| `Remote Pi: Upload Cross Debug` | Sync + upload cross-compiled debug binary |
| `Remote Pi: Upload Cross Release` | Sync + upload cross-compiled release binary |
| `Remote Pi: Run Cross Debug` | Upload + run cross debug on Pi |
| `Remote Pi: Run Cross Release` | Upload + run cross release on Pi |
| `Remote Pi: Run Debug` | Build on Pi + run |

---

## Layout

```
scenes/          scene JSON files and runtime render settings
shaders/         GLSL fragment shaders
resources/
  fonts/         TTF/OTF fonts (note labels, icon atlas)
  textures/      background images
  videos/        playback clips
include/cockscreen/
    app/           CLI and startup helpers
  core/          ControlFrame, ModulationBus
  runtime/       Application, ShaderVideoWindow, Scene, etc.
src/
    app/           Arguments, startup path helpers, DeviceDiscovery
  core/          ModulationBus
  runtime/       Application, Scene, shader pipeline
cmake/           toolchain files
scripts/         bootstrap scripts
```
scripts/         bootstrap scripts


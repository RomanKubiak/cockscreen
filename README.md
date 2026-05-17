# Cockscreen

A Qt6 / OpenGL ES shader pipeline for Raspberry Pi 3B+ and x86_64 Linux. It captures video, audio, MIDI, and OSC data and drives a real-time GLSL shader chain. Typical uses: live VJing, AV installations, generative visuals.

The bundled `chimeras_breath` shader comes from https://www.shadertoy.com/view/4tGfDW.

- USB video grabber capture (V4L2)
- Shader-based video effects (OpenGL ES 2 / desktop GL 2.1)
- Audio-driven modulation (ALSA loopback / WASAPI on Windows)
- OSC control input (UDP)
- MIDI control input (ALSA sequencer / WinMM on Windows)
- Windows cross-compilation via MinGW-w64
- Analog CV input and precision CV output paths for modular-style control voltage work

## Remote target

```text
ssh://atom@cockscreen
```

Local x86_64 scenes default to a 1024×600 Qt6 window. Pi 3B+ builds use `eglfs` directly on DRM/KMS without a compositor.

---

## Startup

Runtime startup is scene-driven. Launch with `--scene-file <path>` or place the platform scene in `scenes/` next to the executable so `cockscreen` can auto-detect it.

Scene files can be plain JSON or JSONC. JSONC is recommended when you want inline `//` or `/* ... */` comments in presets. Default scene auto-detection prefers `.scene.jsonc` and falls back to `.scene.json`.

The only supported CLI options are:

- `--help`
- `--list-devices`
- `--scene-file <path>`
- `--enable-web-server <url>`
- `--ads-vref <volts>` — ADS1256 reference voltage in volts (default `3.3`; also `COCKSCREEN_ADS1256_VREF_VOLTS`)

### Web control

The Qt shader runtime can expose a small live-control server for scene tweaks while the app is running.

Example:

```bash
./out/build/local-x86_64-debug/cockscreen \
    --scene-file scenes/x86_64-linux.scene.jsonc \
    --enable-web-server http://0.0.0.0:8080
```

Supported bind hosts are `localhost`, `0.0.0.0`, or a numeric IP address. The app only starts the server when `--enable-web-server` is provided; the Pi run scripts and tasks pass `http://0.0.0.0:8080` by default for convenience.

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
| `render_device` | string | Optional DRM/KMS device path override (e.g. `"/dev/fb0"`). Defaults to the first available DRM device when omitted. Typically set on Pi eglfs builds. |
| `geometry` | object | Window size object. Parser default: `{ "width": 1024, "height": 600 }`. |
| `render_target` | object | Optional offscreen render size for `qt-shader`, presented to the window after final compositing. |
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
| `secondary_display` | object | Optional Pi-only compact display output. Supports framebuffer panels and direct SPI SSD1309 OLEDs. |
| `inputs` | object | Container for `video`, `playback`, `audio`, `midi`, and optional nested `background_color` or `background`. |
| `video` | object | Shader layer for the built-in video layer (backward-compatible shorthand for `"layers": { "video": { "type": "video", ... } }`). |
| `playback` | object | Shader layer for the built-in playback layer. |
| `screen` | object | Shader layer for the built-in screen layer. |
| `layers` | object | Dictionary of named layers. Keys are the layer names used in `layer_order` and all mapping `layer` fields. See [Named layers](#named-layers) below. |
| `layer_order` | array | Back-to-front compositing order. Any named layer key is accepted — built-in (`video`, `playback`, `screen`) and user-defined. Duplicates and unknown names are silently ignored. |
| `midi_cc_mappings` | array | MIDI CC to shader uniform mappings. |
| `midi_note_mappings` | array | MIDI note to shader uniform mappings. |
| `osc_mappings` | array | OSC address to shader uniform mappings. |
| `shader_uniforms` | array | Static float shader uniform values applied before MIDI/OSC/runtime overrides. |

### Parser defaults and clamping

| Value | Parser behavior |
|---|---|
| `geometry.width`, `geometry.height`, top-level `width`, `height` | Clamped to minimum `1`. |
| `render_target.width`, `render_target.height` | Clamped to minimum `1`. Defaults to `geometry`. |
| colour channels `r/g/b/a` and aliases `red/green/blue/alpha` | Clamped to `[0, 1]`. |
| `inputs.*.scale` | Clamped to minimum `0.01`. Default `1.0`. |
| `inputs.*.animation.speed` | Clamped to minimum `0.0`. Default `1.0`. |
| `video.opacity`, `playback.opacity`, `screen.opacity` | Clamped to `[0, 1]`. Default `1.0`. |
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
| `render_target.presentation` | `stretch`, `fit`, `fill`, `center`, `integer-scale` | Controls how the offscreen texture is presented to the real window. Unknown values fall back to `fit`. |
| `render_target.filter` | `linear`, `nearest` | Texture filtering for the final upscale. `nearest` is useful for pixel-art or hard-edged low-res looks. |
| `inputs.*.animation.preset` | `rotate`, `resize`, `move-x`, `move-y`, `orbit`, `wobble`, `bounce` | Unknown values disable the animation block. Presets are evaluated by `qt-shader`; `v4l2-dmabuf-egl` applies them to the video quad. |
| `layer_order[*]` | any named layer key | Any non-empty string matching a built-in (`video`, `playback`, `screen`) or user-defined (`layers` dict) layer name. Duplicates and unknown names are silently ignored. |
| `pink_key.audio_algorithm` | `0`, `1`, `2`, `3`, `4`, `5` | `0` bass focus, `1` low-mid, `2` high-mid, `3` high, `4` spectral centroid, `5` full-spectrum average. |

### `geometry`

```json
"geometry": {
    "width": 1024,
    "height": 600
}
```

Use `render_target` when the physical display should stay at its native mode but shaders should run at a lower resolution:

```json
"render_target": {
    "enabled": true,
    "width": 640,
    "height": 480,
    "presentation": "fit",
    "filter": "linear"
}
```

`geometry` remains the actual app/window size. `render_target` is the internal shader/composite size. This is useful on small HDMI panels that do not scale lower HDMI modes cleanly.

### `secondary_display`

```json
"secondary_display": {
    "enabled": true,
    "device": "/dev/i2c-1",
    "model": "sparkfun-transparent-ssd1309",
    "interface": "i2c",
    "width": 128,
    "height": 64,
    "rotation_degrees": 0,
    "i2c_address": "0x3c",
    "gpio_reset": -1,
    "render_target": { "enabled": true, "width": 128, "height": 64, "filter": "nearest" },
    "default_page": "linux_os_stats",
    "controls": {}
}
```

For SparkFun’s transparent graphical OLED on Qwiic/I2C, use the SSD1309 `i2c` profile above. The default address is `0x3c` (decimal `60`), and `gpio_reset` is optional when wired. The parser accepts either numeric addresses or strings like `"0x3c"`.

For 4-wire SPI, use `"interface": "spidev"`, `"device": "/dev/spidev0.0"`, `"spi_speed_hz": 8000000`, and a required `"gpio_dc"` BCM GPIO. The board is 128x64, monochrome, 3.3V-only, and SparkFun documents that 128x56 pixels are transparent. Existing secondary pages automatically switch to compact 128x64 monochrome layouts.

The older framebuffer path still works by using `"interface": "framebuffer"` or omitting `interface`, with `"device": "/dev/fb1"` and a framebuffer model such as `"waveshare-1.3inch-lcd-hat"`.

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
        "position": { "x": 0.5, "y": 0.5 },
        "rotation": 0.0,            // degrees, applied around the quad centre
        "animation": {
            "enabled": true,
            "preset": "move-x",
            "speed": 0.25,          // cycles per second
            "amount": 0.15,         // normalized intensity
            "phase": 0.0
        }
    },
    "playback": {
        "enabled": true,
        "file": "videos/clip.mp4",  // relative to resources_directory
        "scale": 0.28,
        "position": { "x": 0.02, "y": 0.02 },
        "rotation": -5.0,
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
| `position` | object | `{ "x": 0, "y": 0 }` | Preferred form. |
| `position_x`, `position_y` | float | `0.0` | Legacy fallback when `position` object is absent. |
| `rotation` | float | `0.0` | Degrees. Applied around the video quad centre by `qt-shader` and `v4l2-dmabuf-egl`. |
| `animation.enabled` | bool | `false` | Enables time-based transform animation. |
| `animation.preset` | string | `""` | One of `rotate`, `resize`, `move-x`, `move-y`, `orbit`, `wobble`, or `bounce`. |
| `animation.speed` | float | `1.0` | Cycles per second for oscillating presets; rotations use full turns per second. |
| `animation.amount` | float | `0.0` | Preset intensity. `resize` is multiplicative; movement presets offset normalized position. |
| `animation.phase` | float | `0.0` | Starting phase, in cycles. |
| `animation.axis` | string | `""` | Optional hint string passed to the animation implementation (e.g. `"x"`, `"y"`, `"z"`). Effect depends on the preset. |
| `transform.animation` | object | omitted | Accepted alias shape for nesting animation under a `transform` block. |
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

#### `inputs.playback.loopback`

Optional GStreamer + tc-netem re-encode pipeline that routes the decoded video through a local RTP/UDP loopback with configurable network impairments before the GL pipeline sees it. Requires `CAP_NET_ADMIN` (run as root or via `sudo`).

```json
"loopback": {
    "enabled":         true,
    "use_appsink":     true,
    "udp_port":        5004,
    "loss_percent":    10.0,
    "corrupt_percent": 40.0,
    "delay_ms":        30,
    "reorder_percent": 20.0,
    "loopback_device": "/dev/video10"
}
```

| Field | Type | Default | Description |
|---|---|---|---|
| `enabled` | bool | `false` | Activates the loopback pipeline. When `false`, playback is direct — no re-encode, no netem. |
| `use_appsink` | bool | `false` | Receiver backend. `true` = in-process GStreamer appsink (H.264 over RTP/UDP → `avdec_h264` → `QVideoSink`); supports `start_ms`/loop and glitch artifacts. `false` = v4l2loopback path (requires the kernel module). |
| `udp_port` | integer | `5004` | Local UDP port for the loopback RTP stream. Sender writes, receiver binds. |
| `loss_percent` | number | `0.0` | Percentage of UDP packets dropped by tc-netem. Dropped packets produce brief freeze holds. Range: 0–100. |
| `corrupt_percent` | number | `0.0` | Percentage of packets given random bit-flips by tc-netem. With H.264 (`use_appsink=true`) this corrupts P-frame bitstreams, causing `avdec_h264` to output garbled macroblocks that persist visually until the next IDR (every ~2 s at default settings). Effective visible-artifact range: 20–60. |
| `delay_ms` | integer | `0` | Fixed latency in milliseconds added to the `lo` interface. Must be `> 0` for `reorder_percent` to have any effect. |
| `reorder_percent` | number | `0.0` | Percentage of packets reordered (out-of-sequence delivery). Requires `delay_ms > 0`. Stresses the RTP jitter buffer; can cause reference-frame mismatches in H.264. Range: 0–100. |
| `loopback_device` | string | `"/dev/video10"` | v4l2loopback device path. Used only when `use_appsink` is `false`. Load with: `sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="cockscreen-lb"`. |

**Codec path (x86_64, `use_appsink=true`):**

```
filesrc → decodebin → videoconvert → videoscale (320×180) → videorate
  → x264enc (tune=zerolatency, key-int-max=60, 800 kbps)
  → rtph264pay (SPS+PPS inlined per IDR) → udpsink :PORT (sync=true)
       ↓ tc-netem: corrupt N%, loss N%, delay Nms, reorder N%
  udpsrc → rtph264depay → h264parse
  → avdec_h264 (output-corrupt=true, discard-corrupted-frames=false)
  → videoconvert → BGRx → appsink (pull thread) → QVideoSink → GL
```

### Named layers

The `layers` dictionary lets you define any number of named compositing layers beyond the three built-in keys (`video`, `playback`, `screen`). Each entry is referenced by its key name everywhere a `layer` field is accepted — `layer_order`, `midi_cc_mappings`, `osc_mappings`, `shader_uniforms`.

```jsonc
"layers": {
    "camera2": {
        "type": "video",        // "video" | "playback" | "screen"  (default "screen")
        "enabled": true,
        "opacity": 1.0,
        "shaders": ["pink_key.glsl"],
        "rect": { "x": 0.5, "y": 0.5, "w": 0.5, "h": 0.5 },
        "input": {              // inline SceneInput — same fields as inputs.video / inputs.playback
            "enabled": true,
            "device": "/dev/video2",
            "format": "qvga"
        }
    },
    "clip2": {
        "type": "playback",
        "enabled": false,
        "opacity": 0.8,
        "shaders": [],
        "transform": {
            "scale": 0.3,
            "position": { "x": 0.1, "y": 0.9 }
        },
        "input": {
            "file": "videos/overlay.mp4",
            "loop_repeat": 0
        }
    }
}
```

Extra fields on each named layer entry:

| Field | Type | Default | Description |
|---|---|---|---|
| `type` | string | `"screen"` | Input pipeline type. `"video"` opens a V4L2 camera; `"playback"` opens a media file; `"screen"` has no input. |
| `input` | object | omitted | Inline `SceneInput` for `video` and `playback` types. Accepts all the same fields as `inputs.video` / `inputs.playback`. |

The built-in `video`, `playback`, and `screen` top-level keys are still parsed for backward compatibility. The `layers` dict entries override them in rendering when the same name is used.

### Layer transform — `rect` (absolute positioning)

Every layer (`video`, `playback`, `screen`, and named layers) accepts a `rect` block as an alternative to `scale` + `position`. When `rect` is present it defines an absolute normalized viewport rectangle and overrides `scale` and `position` entirely.

```jsonc
// Inside a layer or inside a "transform" block — both are accepted.
"rect": {
    "x": 0.0,   // left edge, 0–1 (clamped)
    "y": 0.0,   // top edge,  0–1 (clamped)
    "w": 1.0,   // width,     0–1 (clamped)
    "h": 1.0    // height,    0–1 (clamped)
}
```

The coordinates are normalized: `(0, 0)` is the top-left of the viewport and `(1, 1)` is the bottom-right. `"rect"` can sit directly on the layer object or nested under a `"transform"` key — both are equivalent. When absent, the usual `scale` / `position` / `rotation` transform applies.

### `layer_order`

Lists named layer keys from back to front. Any combination of built-in and user-defined names is accepted. Duplicate or unknown names are silently ignored. Omitting `layer_order` falls back to runtime-default ordering.

```jsonc
"layer_order": [
    "screen",    // drawn first (furthest back)
    "clip2",
    "camera2",
    "playback",
    "video"      // drawn last (on top)
]
```

### Shader layers

Three built-in composited layers: `video`, `playback`, and `screen`. Each has an ordered list of GLSL shaders applied as a chain — the output of one becomes `u_texture` for the next.

```json
"video":    { "enabled": true,  "shaders": ["pink_key.glsl", "video_sphere.glsl"] },
"playback": { "enabled": false, "shaders": ["pixelize_loop.glsl"] },
"screen":   { "enabled": true,  "shaders": ["wireframe_sphere.glsl"] }
```

Use `layer_order` to control compositing order. See [Named layers](#named-layers) above for the full `layer_order` reference, including user-defined names.

Each layer object accepts these fields during parsing:

| Field | Type | Default | Notes |
|---|---|---|---|
| `enabled` | bool | `true` | |
| `opacity` | float | `1.0` | Final layer opacity, clamped to `[0, 1]`. |
| `shaders` | array of strings | `[]` | Non-string entries are ignored. |
| `background_color` / `background` | object | black | Per-layer clear colour before shader chain. |
| `background_image` | object or string | omitted | Per-layer background image, same shape as top-level `background_image`. |
| `transform` | object | omitted | Nested block for `scale`, `position`, `rotation`, `animation`, `rect`. All fields are also accepted directly on the layer object. |
| `rect` | object | omitted | Absolute normalized viewport rect `{x, y, w, h}`. Overrides `scale` + `position` when present. See [Layer transform — rect](#layer-transform--rect-absolute-positioning). |
| `scale` | float | input default | Layer scale. Clamped to minimum `0.01`. |
| `position` | object `{x, y}` | input default | Normalized position. Also accepted as `position_x` / `position_y`. |
| `rotation` | float | `0.0` | Degrees, applied around the layer centre. |
| `animation` | object | omitted | Time-based transform animation. Same fields as `inputs.*.animation`. |

### `shader_uniforms`

Set fixed float uniforms for a shader stage:

```json
"shader_uniforms": [
    {
        "layer":   "screen",
        "shader":  "wireframe_plane.glsl",
        "uniform": "u_plane_audio_speed_mod",
        "value":   1.4
    }
]
```

Parser fields for each shader uniform:

| Field | Type | Default | Notes |
|---|---|---|---|
| `layer` | string | `""` | Required for the uniform to be kept. |
| `shader` | string | `""` | Optional filename or partial shader path match. |
| `uniform` | string | `""` | Required for the uniform to be kept. |
| `value` | float | `0.0` | Applied before MIDI, OSC, and web/runtime overrides. |

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
- `inputs.video.position.x`
- `inputs.video.position.y`
- `inputs.playback.enabled`
- `inputs.playback.file`
- `inputs.playback.scale`
- `inputs.playback.position.x`
- `inputs.playback.position.y`
- `inputs.playback.start_ms`
- `inputs.playback.loop_start_ms`
- `inputs.playback.loop_end_ms`
- `inputs.playback.loop_repeat`
- `inputs.playback.playback_rate`
- `inputs.playback.playback_rate_looping`
- `inputs.playback.loopback.enabled`
- `inputs.playback.loopback.use_appsink`
- `inputs.playback.loopback.udp_port`
- `inputs.playback.loopback.loss_percent`
- `inputs.playback.loopback.corrupt_percent`
- `inputs.playback.loopback.delay_ms`
- `inputs.playback.loopback.reorder_percent`
- `inputs.playback.loopback.loopback_device`
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
- `shader_uniforms[].layer`
- `shader_uniforms[].shader`
- `shader_uniforms[].uniform`
- `shader_uniforms[].value`
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

Analog OSC addresses published by the ADS1256 monitor (Pi only):

- `/analog/ad0` … `/analog/ad7` — normalized [0, 1] value for each ADC channel

### Analog front end

The Pi AARCH64 analog wiring, mux pinout, gate inputs, power distribution, and precision CV output stage now live in [docs/ANALOG.md](docs/ANALOG.md).
The split schematics are [docs/cv-power-input-stage.svg](docs/cv-power-input-stage.svg) and [docs/cv-output-stage.svg](docs/cv-output-stage.svg).
The matching hardware bill of materials is in [docs/BOM.md](docs/BOM.md).

### ADS1256 analog CV input (Pi aarch64 only)

The runtime includes a background driver for the **Waveshare High-Precision AD/DA board** (ADS1256). It continuously reads up to 8 ADC channels and publishes each one as a normalized [0, 1] OSC value at `/analog/ad0` … `/analog/ad7`. Those addresses are then routed to any shader uniform via `osc_mappings` in the scene file — no extra glue code is needed.

#### CLI argument

| Argument | Description |
|---|---|
| `--ads-vref VOLTS` | Reference voltage in volts. Overrides `COCKSCREEN_ADS1256_VREF_VOLTS`. Default: `3.3`. |

#### Environment variables

| Variable | Type | Default | Description |
|---|---|---|---|
| `COCKSCREEN_ADS1256_VREF_VOLTS` | float | `3.3` | Reference voltage. Set to match your board's VREF (e.g. `2.5` for the on-board precision reference, `3.3` for VCC-referenced wiring). Also settable with `--ads-vref`. |
| `COCKSCREEN_ADS1256_PERIOD_MS` | integer | `0` | Sampling interval in milliseconds. `0` = continuous back-to-back DRDY-paced scans (~80 ms round-trip for all 8 channels at 100 SPS). Set to e.g. `200` for a timed interval instead. |
| `COCKSCREEN_ADS1256_CHANNEL` | integer 0–7 | `0` | Which single ADC channel to read when the CD74HC4067 mux is not active. Ignored when `COCKSCREEN_ADS1256_MUX_CHANNELS > 1`. |
| `COCKSCREEN_ADS1256_MUX_CHANNELS` | integer 1–16 | `1` | Set to `> 1` to activate the CD74HC4067 16-channel mux on BCM5/6/13/26. Channels 0…N-1 are scanned in order on each pass. |
| `COCKSCREEN_ADS1256_GATE_POLL_MS` | integer 0–10000 | `20` | How often (ms) the three digital gate inputs (BCM16/19/20) are polled. |

#### GPIO wiring (BCM numbering)

| Signal | BCM pin |
|---|---|
| CS (chip select) | 22 |
| RESET | 18 |
| PWDN (power-down) | 27 |
| DRDY (data ready) | 17 |
| CD74HC4067 S0 | 5 |
| CD74HC4067 S1 | 6 |
| CD74HC4067 S2 | 13 |
| CD74HC4067 S3 | 26 |
| Gate input G0 | 16 |
| Gate input G1 | 19 |
| Gate input G2 | 20 |

#### Routing analog values to shaders

Analog readings arrive as OSC messages internally, so they use the standard `osc_mappings` block in the scene file:

```jsonc
"osc_mappings": [
    {
        /* AD0 potentiometer → wireframe grid speed */
        "address": "/analog/ad0",
        "layer": "screen",
        "shader": "wireframe_plane.glsl",
        "uniform": "u_plane_base_speed",
        "min": 0.0,
        "max": 1.0,
        "exponent": 1.5   // >1 = logarithmic feel (fine control at low end)
    },
    {
        /* AD7 potentiometer → custom uniform */
        "address": "/analog/ad7",
        "layer": "screen",
        "shader": "psychotherapy.glsl",
        "uniform": "u_max_red",
        "min": 0.0,
        "max": 0.95
    }
]
```

The value arriving on a mapped address is clamped to `[0, 1]`, exponent-mapped, then scaled to `[min, max]` before being applied to the uniform — identical to any other OSC mapping.

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
| `u_audio_beat` | `float` | Beat/onset pulse 0–1, derived from audio energy and low-frequency transients. |
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

### `wireframe_plane.glsl`
Synthwave-style perspective floor grid over the previous stage.

**Custom uniforms:**

| Uniform | Range | Default | Description |
|---|---|---|---|
| `u_plane_base_speed` | 0+ | 0.15 | Base grid travel speed. |
| `u_plane_audio_speed_mod` | -1+ | 1.4 | Multiplies speed from RMS, peak, and low FFT energy. Set below `-0.5` to disable. |
| `u_plane_beat_speed_mod` | -1+ | 2.2 | Adds beat/onset kicks to the grid travel and glow. Set below `-0.5` to disable. |

**Audio uniforms used:** `u_audio_rms`, `u_audio_peak`, `u_audio_beat`, `u_audio_fft[16]`, `u_time`

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

### Pi 3B+ native build (via SSH)

```bash
cmake --preset pi-zero2w-debug
cmake --build --preset pi-zero2w-debug
```

### Pi 3B+ cross-compilation

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
| `Remote Pi: Build Sync Run Cross Debug` | Default `F5` flow; cross-build locally, sync source, upload the binary, stop any old `cockscreen`, and run on the Pi |
| `Remote Pi: Run Cross Debug` | Upload + run cross debug on Pi |
| `Remote Pi: Run Cross Release` | Upload + run cross release on Pi |
| `Remote Pi: Run Debug` | Build on Pi + run |
| `Remote Pi: Deploy Scene & Run` | Bound to `F8`; scp `scenes/pizero-linux.scene.jsonc` to the Pi, stop any running `cockscreen`, and restart the cross-debug binary with that scene |

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

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

Local x86_64 builds open a 1024×600 Qt6 window. Pi Zero 2 W builds use `eglfs` directly on DRM/KMS without a compositor.

---

## Configuration

### INI file

Runtime defaults live in `config/x86_64-linux.ini` (desktop) or `config/pi-zero2w.ini` (Pi). Format is Qt `QSettings` INI.

```ini
[runtime]
render_path=qt-shader          ; qt-shader | v4l2-dmabuf-egl
window_title=cockscreen
width=1024
height=600
frame_rate=30
scene_file=../scenes/x86_64-linux.scene.json
shader_directory=../shaders    ; default shader search path (overridden by scene)
```

Config file lookup order:
1. `--config-file <path>` CLI argument
2. `cockscreen.ini` next to the executable
3. `%APPDATA%\cockscreen\config.ini` (Windows)
4. `$HOME/.config/cockscreen/config.ini` (Linux)

---

## Scene file

A scene JSON file controls every visual aspect of a run. Pass it via `scene_file` in the INI or `--scene-file` on the command line.

### Top-level fields

| Field | Type | Description |
|---|---|---|
| `resources_directory` | string | Path (relative to scene file) where fonts, textures, videos, etc. are stored. |
| `shader_directory` | string | Path (relative to scene file) where GLSL shader files are resolved. |
| `note_font_file` | string | Font file (relative to `resources_directory`) used to render MIDI note labels in the atlas. Supports any TTF/OTF loaded by Qt. |
| `show_status_overlay` | bool | Show the diagnostic HUD overlay (FPS, MIDI, audio). Default `false`. |
| `background_color` | object | `{ "r": 0, "g": 0, "b": 0, "a": 1 }` — clear colour between frames. |
| `background_image` | object | See below. |

### `background_image`

```json
"background_image": {
    "file": "textures/my_bg.jpg",
    "placement": "proportional-stretch"
}
```

`placement` values: `center`, `stretched`, `proportional-stretch`, `tiled`.

### `inputs`

```json
"inputs": {
    "video": {
        "enabled": true,
        "device": "/dev/video1",
        "format": "qvga",           // "qvga" (320×240), "vga", "hd" etc.
        "scale": 0.5,               // display scale relative to window
        "on_top": false,            // whether video layer composites above screen layer
        "position": { "x": 0.5, "y": 0.5 }
    },
    "playback": {
        "enabled": true,
        "file": "videos/clip.mp4",  // relative to resources_directory
        "scale": 0.28,
        "on_top": true,
        "position": { "x": 0.02, "y": 0.02 }
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

### Shader layers

Three composited layers: `video`, `playback`, and `screen`. Each has an ordered list of GLSL shaders applied as a chain — the output of one becomes `u_texture` for the next.

```json
"video":    { "enabled": true,  "shaders": ["pink_key.glsl", "video_sphere.glsl"] },
"playback": { "enabled": false, "shaders": ["pixelize_loop.glsl"] },
"screen":   { "enabled": true,  "shaders": ["wireframe_sphere.glsl"] }
```

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

### `midi_note_mappings`

Map MIDI note activity to a shader uniform:

```json
"midi_note_mappings": [
    {
        "layer":   "screen",
        "shader":  "crt_noise.glsl",
        "uniform": "u_note_flash",
        "decay":   0.3   // seconds to decay to 0 after note-off
    }
]
```

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

The OSC server listens on the port set by `osc_endpoint` in the INI file (e.g. `osc_endpoint=0.0.0.0:9000`). Values arriving on a mapped address are clamped to `[0, 1]`, exponent-mapped, then scaled to `[min, max]` before being set as the uniform.

### Analog front end

The Pi AARCH64 analog wiring, mux pinout, gate inputs, power distribution, and precision CV output stage now live in [ANALOG.md](ANALOG.md).
The split schematics are [docs/cv-power-input-stage.svg](docs/cv-power-input-stage.svg) and [docs/cv-output-stage.svg](docs/cv-output-stage.svg).
The matching hardware bill of materials is in [BOM.md](BOM.md).

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

---

## Build

### Local x86_64 Linux (debug)

```bash
cmake --preset local-x86_64-debug
cmake --build --preset local-x86_64-debug
./out/build/local-x86_64-debug/cockscreen --config-file config/x86_64-linux.ini
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
config/          runtime INI files per platform
scenes/          scene JSON files
shaders/         GLSL fragment shaders
resources/
  fonts/         TTF/OTF fonts (note labels, icon atlas)
  textures/      background images
  videos/        playback clips
include/cockscreen/
  app/           CLI and config headers
  core/          ControlFrame, ModulationBus
  runtime/       Application, ShaderVideoWindow, Scene, etc.
src/
  app/           Arguments, Config, DeviceDiscovery
  core/          ModulationBus
  runtime/       Application, Scene, shader pipeline
cmake/           toolchain files
scripts/         bootstrap scripts
```


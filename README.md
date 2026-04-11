# Cockscreen

Initial C++ project scaffold for a Raspberry Pi Zero 2 W remote host. The workflow is to build and test locally on x86_64 Linux first, then deploy and validate on the Pi over SSH while leaving clean seams for:

- USB video grabber capture
- Shader-based video effects
- Audio-driven modulation
- OSC control input
- MIDI control input

Current runtime behavior opens a Qt6 Widgets window with a simple dashboard UI so there is a concrete graphics entry point before the OpenGL stage is added.

## Remote target

Open the repository on the Pi Zero 2 W via Remote - SSH using:

```text
ssh://atom@192.168.41.190
```

The repository supports local x86_64 Linux testing, native builds on the Pi, and a cached cross-compilation flow from x86_64 Linux to the Pi Zero 2 W.

Local x86_64 builds default to a 1024x600 windowed Qt6 UI. The Pi Zero 2 W build uses Qt's `eglfs` platform plugin so it can run directly on DRM/KMS without a compositor.

## Configuration

Runtime defaults can be stored in [config/default.ini](config/default.ini), which uses the Qt `QSettings` INI format. Most values in the file are loaded first and can be overridden by command-line arguments.

The `runtime` group supports the same options as the CLI, including `video_device`, `audio_device`, `osc_endpoint`, `midi_input`, `shader_directory`, `shader_file`, `screen_shader_file`, `top_layer`, `top_layer_opacity`, `render_path`, `window_title`, `width`, `height`, and `frame_rate`.

Scene-specific rendering can be loaded from a separate JSON file via `scene_file`. A scene defines inputs, video and screen shader chains, a background color, a background image, a shader directory, a resources directory, an optional note-font resource, and MIDI CC mappings for shader uniforms. The video input block can also request a capture format and control on-screen scale and position.

Background images are resolved from the scene's `resources_directory` and can be placed with `center`, `stretched`, `proportional-stretch` or `tiled`.

Shader files are resolved from the scene's `shader_directory` first, then the INI `shader_directory`, then the CLI override, then the executable directory.

Scene resources such as fonts, videos, data files, textures, and background images are resolved from the scene's `resources_directory`. The MIDI dot note labels use a font resource from that directory when one is provided.

An example scene is available at [scenes/example.scene.json](scenes/example.scene.json).

The sample scene uses the default output monitor for audio capture and the ALSA sequencer port name `through` as a portable MIDI placeholder.

Formatting is configured with [.clang-format](/home/atom/devel/cockscreen/.clang-format) using the `Microsoft` base style, and workspace save actions are configured to format files automatically in VS Code.

## Layout

- `config/` runtime INI files
- `scenes/` scene JSON files
- `resources/` per-scene assets such as fonts, videos, data, and textures
- `include/cockscreen/` public headers for runtime, control, and pipeline types
- `src/core/` shared runtime state and modulation bus implementation
- `src/runtime/` application entry and orchestration
- `CMakePresets.json` presets for local x86_64 Linux and Pi Zero 2 W builds

## Build

Local x86_64 Linux:

```bash
cmake --preset local-x86_64-debug
cmake --build --preset local-x86_64-debug
```

Pi Zero 2 W remote session:

```bash
cmake --preset pi-zero2w-debug
cmake --build --preset pi-zero2w-debug
```

Pi Zero 2 W cross-compilation from the local x86_64 host:

```bash
./scripts/bootstrap-pizero-cross.sh
cmake --preset cross-pi-zero2w-debug
cmake --build --preset cross-pi-zero2w-debug
```

The bootstrap script downloads the current Arm GNU/Linux AArch64 toolchain, creates an arm64 Debian Trixie sysroot with the Qt6, ALSA, EGL, and OpenGL development packages required by this project, and stores everything under `/work/pizero` for reuse across later builds.

Bootstrap, configure, and build temp files are redirected into [tmp](tmp) inside the workspace rather than `/tmp`.

The default cross sysroot targets Debian 13 Trixie so it matches the Pi's current glibc and Qt 6.8.2 baseline more closely. If the Pi diverges from that baseline later, use a Pi-sourced sysroot overlay or a fully synced Pi sysroot before relying on exact Qt ABI and feature parity.

The generated cross toolchain assets are split like this:

- `/work/pizero/toolchains/current` active Arm GNU toolchain symlink
- `/work/pizero/sysroot` extracted arm64 target sysroot used by CMake
- `/work/pizero/cache/debootstrap` cached `.deb` downloads reused by debootstrap
- `/work/pizero/stage` staging prefix for install-time output if needed

Optimized Pi Zero 2 W release build:

```bash
cmake --preset pi-zero2w-release
cmake --build --preset pi-zero2w-release
```

## VS Code Tasks

The workspace includes these tasks in [.vscode/tasks.json](/home/atom/devel/cockscreen/.vscode/tasks.json):

- `Local: Build Debug`
- `Local: Run Debug`
- `Remote Pi: Sync Workspace`
- `Remote Pi: Build Debug`
- `Remote Pi: Run Debug`
- `Remote Pi: Build Release`
- `Remote Pi: Run Release`

The remote tasks prompt for the SSH host and remote project directory. The default host is `atom@192.168.41.190` and the default remote project path is `/home/atom/cockscreen`.

## Next implementation steps

1. Add a V4L2 or OpenCV capture backend for the USB grabber.
2. Add an OpenGL ES shader pipeline for frame processing.
3. Wire in audio analysis, OSC, and MIDI feeders into the modulation bus.
4. Add runtime configuration and device discovery.

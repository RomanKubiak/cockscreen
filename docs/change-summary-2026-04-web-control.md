# April 2026 change summary

This change set groups the recent runtime and shader work into one repo snapshot.

## Runtime

- added single-pass ShaderToy compatibility for shaders that expose `mainImage(...)`
- added explicit rejection for unsupported multipass or Common-pass ShaderToy features
- added fatal startup and render error presentation in-window instead of failing silently
- added a Qt-based web control server for live scene edits
- added `--enable-web-server URL` so the control server is opt-in and bindable to a chosen address and port

## Web control

- added `/` for the control page, `/api/state` for JSON state, and `/api/apply` for live updates
- added live edits for background colour, background image, layer enable flags, and ordered shader chains
- updated the web UI to support multi-select shader editing, explicit shader ordering, and a mobile-friendly layout

## Shaders

- added `shaders/toy_shader.glsl`
- added `shaders/toy_shader_image.glsl`
- added `shaders/toy_synthwave.glsl`

## Notes

- the local `scenes/x86_64-linux.scene.json` file was intentionally left out of the commit because it currently contains machine-specific device paths and local media choices
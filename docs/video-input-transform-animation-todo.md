# Video Input Transform Animation TODO

Goal: animate the live video input layer's screen transform over time and from external modulation, without requiring shader authors to fake movement in texture coordinates.

Scope:
- Applies first to `inputs.video`.
- Reuse for `inputs.playback` if the design stays generic.
- Supports position, scale, and rotation.
- Works on `qt-shader`.
- Works on `v4l2-dmabuf-egl` for direct video quad transforms.

## Scene Schema

- [x] Add `rotation` to `SceneInput`, in degrees.
- [x] Add `transform` or `animation` block under `inputs.video`.
- [x] Keep existing static fields as the base transform:
  - `scale`
  - `position.x`
  - `position.y`
  - `rotation`
- [ ] Define modulation as additive or multiplicative per property.
- [x] Clamp safe ranges:
  - `scale >= 0.01`
  - `position.x/y` may exceed `[0, 1]` only if offscreen movement is desired
  - `rotation` wraps naturally

Example shape:

```jsonc
"inputs": {
  "video": {
    "scale": 0.3,
    "position": { "x": 0.5, "y": 0.5 },
    "rotation": 0.0,
    "animation": {
      "enabled": true,
      "preset": "orbit",
      "speed": 0.25,
      "amount": 0.2
    }
  }
}
```

## Preset Animations

- [x] `rotate`: continuously rotate around the video quad center.
- [x] `resize`: pulse scale around the base scale.
- [x] `move-x`: oscillate horizontally around the base position.
- [x] `move-y`: oscillate vertically around the base position.
- [x] `orbit`: move in a circular path around the base position.
- [x] `wobble`: combine small position, scale, and rotation offsets.
- [x] `bounce`: move between screen edges using normalized position.

Shared preset fields:

- [x] `enabled`: boolean.
- [x] `preset`: string enum.
- [x] `speed`: cycles per second or normalized multiplier.
- [x] `amount`: normalized intensity.
- [x] `phase`: starting phase offset.
- [ ] `axis`: optional for move-style presets.

## External Modulation

- [ ] Add transform modulation targets independent from shader uniform mappings.
- [ ] Support MIDI CC as a transform source.
- [ ] Support OSC address as a transform source.
- [ ] Support audio level as a transform source.
- [ ] Decide target names:
  - `video.position.x`
  - `video.position.y`
  - `video.scale`
  - `video.rotation`
- [ ] Apply modulation after preset animation.
- [ ] Keep shader uniform mappings unchanged.

Example shape:

```jsonc
"transform_mappings": [
  {
    "source": "osc",
    "address": "/video/x",
    "target": "video.position.x",
    "min": 0.0,
    "max": 1.0,
    "exponent": 1.0
  },
  {
    "source": "midi_cc",
    "channel": 0,
    "cc": 1,
    "target": "video.rotation",
    "min": -30.0,
    "max": 30.0
  }
]
```

## Runtime Data Model

- [x] Add a transform animation struct to `include/cockscreen/runtime/Scene.hpp`.
- [ ] Add transform mapping structs to `Scene.hpp`.
- [x] Parse animation blocks in `src/runtime/scene/Parse.cpp`.
- [ ] Parse transform mappings in `Parse.cpp`.
- [x] Document all new fields in `README.md`.
- [x] Add annotated examples in `scenes/examples/annotated.scene.jsonc`.

## Rendering

- [x] Replace static `video_display_rect(...)` usage with an evaluated transform.
- [ ] Compute evaluated transform each frame from:
  - base scene input
  - elapsed time
  - current `ControlFrame`
  - transform mappings
- [x] Update `draw_textured_quad` to support rotation.
- [x] Rotate around the quad center by default.
- [x] Preserve current behavior when animation is disabled and rotation is `0`.
- [x] Use the same transform evaluator for playback if enabled.

Implementation files:

- [x] `include/cockscreen/runtime/Scene.hpp`
- [x] `src/runtime/scene/Parse.cpp`
- [x] `src/runtime/shadervideo/Support.cpp`
- [x] `include/cockscreen/runtime/shadervideo/Support.hpp`
- [x] `src/runtime/shadervideo/Rendering.cpp`
- [x] `README.md`
- [x] `scenes/examples/annotated.scene.jsonc`

## Web Control

- [ ] Expose base transform fields in the web state.
- [ ] Expose animation enabled/preset/speed/amount/phase.
- [ ] Expose live transform mapping values if useful for debugging.
- [ ] Allow web updates to change animation parameters without restart.

Implementation files:

- [ ] `src/runtime/web/SceneControlServer.cpp`
- [ ] `include/cockscreen/runtime/web/SceneControlServer.hpp`

## Testing

- [ ] Add parser coverage for default static behavior.
- [ ] Add parser coverage for each preset name.
- [ ] Add parser coverage for invalid preset fallback.
- [ ] Add parser coverage for MIDI/OSC transform mappings.
- [ ] Add rendering smoke test or manual test scene.
- [ ] Verify disabled animation is pixel-position compatible with current behavior.
- [ ] Verify rotation with opacity and layer ordering.
- [ ] Verify animation does not break video shader chains.

## Suggested First Cut

- [x] Implement `rotation`.
- [x] Implement `rotate`, `resize`, and `move-x`.
- [x] Implement only time-based animation first.
- [ ] Add OSC/MIDI/audio transform mappings after the base evaluator is stable.

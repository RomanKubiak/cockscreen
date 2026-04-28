#pragma once

#include "Scene.hpp"

#include <cstdint>

namespace cockscreen::runtime
{

// ---------------------------------------------------------------------------
// apply_artifact()
//
// Applies reproducible corruption to a raw pixel buffer in-place.
//
//   data        — pointer to the first byte of row 0.
//   width_bytes — logical bytes per row (pixels * bytes_per_pixel).
//                 For YUYV/UYVY this is frame_width * 2.
//                 For RGB24/BGR24 this is frame_width * 3.
//                 For RGBA8888 this is frame_width * 4.
//   height      — number of rows.
//   stride      — actual bytes between the start of consecutive rows
//                 (may equal width_bytes when storage is packed).
//   params      — effect parameters.
//   frame_index — monotonically incrementing counter; advances the PRNG
//                 state so every frame produces a different but deterministic
//                 corruption pattern for a given seed.
// ---------------------------------------------------------------------------
void apply_artifact(std::uint8_t *data, int width_bytes, int height, int stride,
                    const ArtifactParams &params, std::uint64_t frame_index);

} // namespace cockscreen::runtime

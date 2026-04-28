#include "cockscreen/runtime/ArtifactInjector.hpp"

#include <algorithm>
#include <cstring>

namespace cockscreen::runtime
{

namespace
{

// ---------------------------------------------------------------------------
// xorshift64 — fast, reproducible PRNG (no stdlib dependency, no state object).
// Pass the state by reference; seed with (params.seed ^ (frame_index * 0x9e3779b97f4a7c15)).
// ---------------------------------------------------------------------------
inline std::uint64_t xorshift64(std::uint64_t &state) noexcept
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

// Map an xorshift64 output to [0, range).
inline int rng_int(std::uint64_t &state, int range) noexcept
{
    return static_cast<int>(xorshift64(state) % static_cast<std::uint64_t>(range));
}

// Map an xorshift64 output to [0.0, 1.0).
inline float rng_float(std::uint64_t &state) noexcept
{
    return static_cast<float>(xorshift64(state) & 0xffffff) / static_cast<float>(0x1000000);
}

// ---------------------------------------------------------------------------
// scanline_drop: randomly blank individual rows.
//
// Number of dropped lines scales linearly with strength and height.
// Each chosen row is zeroed (simulates a dropped YUYV / RGB scan line).
// ---------------------------------------------------------------------------
void apply_scanline_drop(std::uint8_t *data, int width_bytes, int height, int stride,
                         float strength, std::uint64_t &rng) noexcept
{
    const int n_drops = std::max(1, static_cast<int>(static_cast<float>(height) * strength * 0.15F));
    for (int i = 0; i < n_drops; ++i)
    {
        const int row = rng_int(rng, height);
        std::memset(data + static_cast<std::ptrdiff_t>(row) * stride, 0,
                    static_cast<std::size_t>(width_bytes));
    }
}

// ---------------------------------------------------------------------------
// block_corrupt: scatter corrupted aligned blocks across the frame.
//
// Each block is filled with a single random byte value, mimicking the
// solid-colour macroblocks typical of lost intra-coded blocks.
// ---------------------------------------------------------------------------
void apply_block_corrupt(std::uint8_t *data, int width_bytes, int height, int stride,
                         float strength, int block_size, std::uint64_t &rng) noexcept
{
    const int blocks_x = std::max(1, width_bytes / block_size);
    const int blocks_y = std::max(1, height / block_size);
    const int total_blocks = blocks_x * blocks_y;
    const int n_corrupt = std::max(1, static_cast<int>(static_cast<float>(total_blocks) * strength * 0.25F));

    for (int i = 0; i < n_corrupt; ++i)
    {
        const int bx = rng_int(rng, blocks_x) * block_size;
        const int by = rng_int(rng, blocks_y) * block_size;
        const auto fill_byte = static_cast<std::uint8_t>(xorshift64(rng) & 0xff);

        const int row_end = std::min(by + block_size, height);
        const int col_end = std::min(bx + block_size, width_bytes);

        for (int row = by; row < row_end; ++row)
        {
            std::memset(data + static_cast<std::ptrdiff_t>(row) * stride + bx,
                        fill_byte,
                        static_cast<std::size_t>(col_end - bx));
        }
    }
}

// ---------------------------------------------------------------------------
// bit_flip: XOR random byte-runs in the buffer with a random mask.
//
// Models RF/cable interference where a burst of bits is inverted.
// ---------------------------------------------------------------------------
void apply_bit_flip(std::uint8_t *data, int width_bytes, int height, int stride,
                    float strength, std::uint64_t &rng) noexcept
{
    // Number of burst events per frame.
    const int n_bursts = std::max(1, static_cast<int>(static_cast<float>(height) * strength * 0.05F));
    const int max_burst_len = std::max(8, static_cast<int>(static_cast<float>(width_bytes) * 0.04F));

    for (int i = 0; i < n_bursts; ++i)
    {
        const int row = rng_int(rng, height);
        const int offset = rng_int(rng, width_bytes);
        const int burst_len = rng_int(rng, max_burst_len) + 1;
        const auto mask = static_cast<std::uint8_t>(xorshift64(rng) & 0xff);

        std::uint8_t *row_ptr = data + static_cast<std::ptrdiff_t>(row) * stride;
        const int end = std::min(offset + burst_len, width_bytes);
        for (int b = offset; b < end; ++b)
        {
            row_ptr[b] ^= mask;
        }
    }
}

// ---------------------------------------------------------------------------
// smear: shift bytes within a row forward by a random displacement.
//
// Models a sync-loss glitch where the decoder misaligns its pixel pointer.
// The displaced region is overwritten; the vacated tail is zeroed.
// ---------------------------------------------------------------------------
void apply_smear(std::uint8_t *data, int width_bytes, int height, int stride,
                 float strength, std::uint64_t &rng) noexcept
{
    const int n_rows = std::max(1, static_cast<int>(static_cast<float>(height) * strength * 0.08F));
    const int max_shift = std::max(4, static_cast<int>(static_cast<float>(width_bytes) * 0.12F));

    for (int i = 0; i < n_rows; ++i)
    {
        const int row = rng_int(rng, height);
        const int shift = rng_int(rng, max_shift) + 1;
        std::uint8_t *row_ptr = data + static_cast<std::ptrdiff_t>(row) * stride;

        // Shift bytes left by `shift` positions (memmove within the row).
        const int copy_len = width_bytes - shift;
        if (copy_len > 0)
        {
            std::memmove(row_ptr, row_ptr + shift, static_cast<std::size_t>(copy_len));
            std::memset(row_ptr + copy_len, 0, static_cast<std::size_t>(shift));
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point.
// ---------------------------------------------------------------------------
void apply_artifact(std::uint8_t *data, int width_bytes, int height, int stride,
                    const ArtifactParams &params, std::uint64_t frame_index)
{
    if (!params.enabled || data == nullptr || width_bytes <= 0 || height <= 0)
    {
        return;
    }

    // Seed: mix the user seed with the frame counter so each frame is unique
    // but the sequence is fully reproducible for a given seed.
    std::uint64_t rng = static_cast<std::uint64_t>(params.seed) ^
                        (frame_index * UINT64_C(0x9e3779b97f4a7c15));
    // Warm up the PRNG.
    xorshift64(rng);
    xorshift64(rng);

    if (params.scanline_drop)
    {
        apply_scanline_drop(data, width_bytes, height, stride, params.strength, rng);
    }

    if (params.block_corrupt)
    {
        apply_block_corrupt(data, width_bytes, height, stride, params.strength, params.block_size, rng);
    }

    if (params.bit_flip)
    {
        apply_bit_flip(data, width_bytes, height, stride, params.strength, rng);
    }

    if (params.smear)
    {
        apply_smear(data, width_bytes, height, stride, params.strength, rng);
    }
}

} // namespace cockscreen::runtime

// font_awesome_burst.glsl
// Renders 3 rows × 5 Font Awesome icons in the centre of the viewport.
// Icons cycle rapidly (multiple times per second) using a hash-based random selection.
// The background is left visible everywhere except where the glyph alpha is opaque.

precision mediump float;

uniform sampler2D u_texture;    // background (slot 0)
uniform sampler2D u_icon_atlas; // FA icon atlas (slot 2)
uniform vec2 u_icon_grid;       // vec2(8.0, 8.0) — columns × rows in the atlas
uniform vec2 u_viewport_size;
uniform float u_time;

varying vec2 v_texcoord;

float random(vec2 st)
{
    return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123);
}

// ----- hash ----------------------------------------------------------------
float hash11(float n)
{
    return fract(sin(n) * 43758.5453123);
}

// ----- atlas UV for a given icon slot (0..63) -----------------------------
vec2 icon_cell_origin(float icon_idx)
{
    float col = mod(icon_idx, u_icon_grid.x);
    float row = floor(icon_idx / u_icon_grid.x);
    return vec2(col, row) / u_icon_grid;
}

vec4 sample_icon(float icon_idx, vec2 local_uv)
{
    // local_uv in [0,1] within the cell
    vec2 cell_size = 1.0 / u_icon_grid;
    vec2 atlas_uv = icon_cell_origin(icon_idx) + local_uv * cell_size;
    return texture2D(u_icon_atlas, atlas_uv);
}

void main()
{
    const int kWords = 3;
    const int kCharsWord = 5;
    const float kIconCount = 64.0;

    // Integer second counter — confirmed working from background pulse test
    float t = floor(u_time * 10.0);

    float aspect = u_viewport_size.x / max(u_viewport_size.y, 1.0);
    float glyph_h = 0.15;
    float glyph_w = glyph_h / aspect;

    const float kWordGap = 0.025;
    float block_h = float(kWords) * glyph_h + float(kWords - 1) * kWordGap;
    float block_w = float(kCharsWord) * glyph_w;

    float block_x = 0.5 - block_w * 0.5;
    float block_y = 0.5 - block_h * 0.5;

    vec2 uv = vec2(v_texcoord.x, 1.0 - v_texcoord.y);

    vec4 bg = texture2D(u_texture, v_texcoord);
    vec4 out_color = bg;

    for (int w = 0; w < kWords; w++)
    {
        float word_top = block_y + float(w) * (glyph_h + kWordGap);
        for (int c = 0; c < kCharsWord; c++)
        {
            float char_left = block_x + float(c) * glyph_w;

            float in_x = step(char_left, uv.x) * step(uv.x, char_left + glyph_w);
            float in_y = step(word_top, uv.y) * step(uv.y, word_top + glyph_h);
            if (in_x * in_y == 0.0)
            {
                continue;
            }

            // Each slot starts at a different offset (prime-spaced) so they all
            // show different icons and cycle independently. Pure modulo — no sin().
            float slot = float(w * kCharsWord + c);
            float idx = mod(t + slot * 7.0, kIconCount);

            vec2 local = (uv - vec2(char_left, word_top)) / vec2(glyph_w, glyph_h);
            vec4 px = sample_icon(idx, local);
            out_color = mix(out_color, vec4(1.0), px.a);
        }
    }

    gl_FragColor = out_color;
}

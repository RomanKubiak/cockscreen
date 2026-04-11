#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform vec2 u_viewport_size;
uniform float u_time;
uniform float u_note_glitch;
uniform float u_note_flash;
uniform sampler2D u_texture;

float random(vec2 st)
{
    return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123);
}

// CRT Curvature
vec2 curve(vec2 uv)
{
    uv = (uv - 0.5) * 2.0;
    uv.x *= 1.0 + pow((abs(uv.y) / 5.0), 2.0);
    uv.y *= 1.0 + pow((abs(uv.x) / 4.0), 2.0);
    uv = (uv / 2.0) + 0.5;
    return uv;
}

void main()
{
    // 1. Coordinates & Curvature
    vec2 st = curve(v_texcoord);
    float time = u_time;
    vec4 base = texture2D(u_texture, st);

    // Out of bounds check (Bezel)
    if (st.x < 0.0 || st.x > 1.0 || st.y < 0.0 || st.y > 1.0)
    {
        gl_FragColor = vec4(0.0, 0.0, 0.0, base.a);
        return;
    }

    // 2. VHS Tracking Logic
    float band_y = fract(time * (0.2 + u_note_glitch * 0.35));
    float in_band = step(band_y - 0.1, st.y) * step(st.y, band_y + 0.1);

    // Jitter amount
    float jitter = (random(vec2(time, st.y)) - 0.5) * (0.002 + u_note_glitch * 0.006);
    jitter += in_band * (random(vec2(st.y, time)) - 0.5) * (0.02 + u_note_glitch * 0.05);

    vec2 uv = vec2(st.x + jitter, st.y);

    // 3. Chromatic Aberration (Texture Sampling)
    // We sample the actual texture three times with offsets
    float texel_x = 1.0 / max(u_viewport_size.x, 1.0);
    float r = texture2D(u_texture, uv + vec2(texel_x, 0.0)).r;
    float g = texture2D(u_texture, uv).g;
    float b = texture2D(u_texture, uv - vec2(texel_x, 0.0)).b;
    vec3 color = vec3(r, g, b);

    // 4. Analog Artifacts
    // Static Noise in the tracking band
    color += in_band * random(uv + time) * (0.15 + u_note_glitch * 0.35);

    // Scanlines
    float scanline = sin(st.y * u_viewport_size.y * (1.5 + u_note_glitch * 0.6)) * (0.08 + u_note_glitch * 0.04);
    color -= scanline;

    // Phosphor Mask (Fine vertical grain)
    if (mod(gl_FragCoord.x, 2.0) < 1.0)
        color *= 0.95;

    // CRT Flicker
    color *= 0.98 + 0.02 * sin(110.0 * time + u_note_glitch * 6.0);

    // Note-triggered burst
    color += vec3(0.18, 0.06, 0.02) * u_note_flash;

    // Vignette
    float vignette = 16.0 * st.x * st.y * (1.0 - st.x) * (1.0 - st.y);
    color *= pow(vignette, 0.15);

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
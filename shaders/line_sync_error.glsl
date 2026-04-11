#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform vec2 u_viewport_size;
uniform float u_time;
uniform sampler2D u_texture;

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float line_noise(float line_index, float phase)
{
    return hash11(line_index * 0.073 + phase * 17.13);
}

void main()
{
    vec2 uv = v_texcoord;
    float viewport_height = max(u_viewport_size.y, 1.0);
    float viewport_width = max(u_viewport_size.x, 1.0);
    float scanline = floor(uv.y * viewport_height);
    float time = u_time;

    // A rolling sync-loss band that drags horizontal lines out of alignment.
    float tear_center = fract(time * 0.11 + sin(time * 0.37) * 0.06);
    float tear_distance = abs(uv.y - tear_center);
    float tear_mask = smoothstep(0.14, 0.0, tear_distance);

    float phase = floor(time * 10.0);
    float jitter = (line_noise(scanline, phase) - 0.5) * 0.008;
    float group_noise = (line_noise(floor(scanline / 3.0), phase + 5.0) - 0.5) * 0.018;
    float drift = sin(uv.y * 1450.0 + time * 18.0) * 0.0025;

    float tearing_wave = sin((uv.y - tear_center) * 220.0 - time * 24.0);
    float tearing_shift = tearing_wave * (0.01 + tear_mask * 0.08);
    float hold_pull = (line_noise(scanline + phase * 13.0, phase + 2.0) - 0.5) * tear_mask * 0.12;

    float offset_x = drift + jitter + group_noise + tearing_shift + hold_pull;
    float shifted_x = uv.x + offset_x;
    float wrapped_x = fract(shifted_x);

    vec2 sample_uv = vec2(wrapped_x, clamp(uv.y + sin(time * 6.0) * tear_mask * 0.002, 0.0, 1.0));
    float texel_x = 1.0 / viewport_width;

    // Slight channel separation helps sell the analog mis-sync.
    float r = texture2D(u_texture, sample_uv + vec2(texel_x * (0.5 + tear_mask * 1.5), 0.0)).r;
    float g = texture2D(u_texture, sample_uv).g;
    float b = texture2D(u_texture, sample_uv - vec2(texel_x * (0.5 + tear_mask * 1.5), 0.0)).b;
    vec4 base = texture2D(u_texture, sample_uv);
    vec3 color = vec3(r, g, b);

    // Darken the wrap seam and add a noisy retrace band where the sync slips hardest.
    float wrapped = step(shifted_x, 0.0) + step(1.0, shifted_x);
    float seam = wrapped * (0.18 + tear_mask * 0.45);
    float retrace = smoothstep(0.0, 0.008, abs(fract(shifted_x) - 0.5)) * tear_mask * 0.06;
    color *= 1.0 - seam;
    color += (line_noise(scanline, phase + 11.0) - 0.5) * tear_mask * 0.09;
    color -= retrace;

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
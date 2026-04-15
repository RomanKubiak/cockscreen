precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform vec2 u_resolution;

const float kTau = 6.28318530718;

vec3 palette(float t)
{
    return 0.5 + 0.5 * cos(kTau * (vec3(0.02, 0.29, 0.56) + t));
}

void main()
{
    vec2 uv = v_texcoord;
    vec4 base = texture2D(u_texture, uv);

    float bass = (u_audio_fft[0] + u_audio_fft[1] + u_audio_fft[2] + u_audio_fft[3]) * 0.25;
    float mids = (u_audio_fft[4] + u_audio_fft[5] + u_audio_fft[6] + u_audio_fft[7] + u_audio_fft[8] + u_audio_fft[9] +
                  u_audio_fft[10] + u_audio_fft[11]) *
                 0.125;
    float highs = (u_audio_fft[12] + u_audio_fft[13] + u_audio_fft[14] + u_audio_fft[15]) * 0.25;
    float audio_drive = clamp(u_audio_rms * 1.35 + u_audio_peak * 0.95, 0.0, 1.0);

    float aspect = u_resolution.x > 0.5 ? (u_resolution.x / max(u_resolution.y, 1.0)) : 1.7777778;
    vec2 p = (uv - vec2(0.5)) * vec2(aspect, 1.0);
    float radius = length(p);
    float angle = atan(p.y, p.x);

    float spiral = sin(radius * (18.0 + bass * 14.0) - u_time * (4.0 + highs * 9.0) +
                       sin(angle * 6.0 + u_time * (1.3 + mids * 2.5)) * (1.4 + bass * 1.6));
    float bands = sin((p.x + p.y) * (9.0 + mids * 4.0) + u_time * (2.2 + bass * 5.0));
    float cross = sin(p.x * (12.0 + highs * 6.0) - u_time * (3.0 + mids * 4.0)) *
                  sin(p.y * (13.0 + bass * 5.0) + u_time * (2.7 + highs * 4.0));
    float ripple = sin(angle * 9.0 - u_time * (2.5 + bass * 2.0) + radius * (10.0 + highs * 12.0));
    float plasma = spiral + bands + cross + ripple;

    float palette_phase = plasma * 0.14 + u_time * (0.05 + highs * 0.04) + bass * 0.12;
    vec3 plasma_color = palette(palette_phase);
    plasma_color = mix(plasma_color, plasma_color.gbr, smoothstep(0.22, 0.92, mids + highs * 0.35) * 0.22);

    vec2 warp = vec2(sin(p.y * 10.0 + plasma * 0.75 + u_time * (1.4 + highs * 3.5)),
                     cos(p.x * 11.0 - plasma * 0.70 - u_time * (1.2 + bass * 4.2))) *
                (0.006 + audio_drive * 0.018);
    vec2 uv_r = clamp(uv + warp * vec2(1.30, 0.90), 0.0, 1.0);
    vec2 uv_b = clamp(uv - warp * vec2(0.85, 1.15), 0.0, 1.0);
    vec3 refracted = vec3(texture2D(u_texture, uv_r).r, base.g, texture2D(u_texture, uv_b).b);

    float plasma_mask = 0.5 + 0.5 * sin(plasma * 0.9);
    float glow = smoothstep(0.28, 1.0, plasma_mask) * (0.7 + audio_drive * 0.7);
    float vignette = smoothstep(1.28, 0.18, radius);
    float flash = smoothstep(0.55, 1.0, u_audio_peak);

    vec3 backdrop = mix(base.rgb, refracted, 0.38 + audio_drive * 0.26);
    vec3 color = backdrop * (0.72 + 0.18 * vignette);
    color += plasma_color * (0.32 + 0.42 * plasma_mask) * vignette;
    color += plasma_color.brg * glow * (0.10 + highs * 0.30 + flash * 0.18);

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
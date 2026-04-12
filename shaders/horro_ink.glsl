precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;

float getLuma(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

// Quick pseudo-random hash for per-frame flicker
float hash(float n)
{
    return fract(sin(n) * 43758.5453123);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec2 uv = v_texcoord;

    // 1. JITTER & SHAKE — multi-frequency so it feels erratic
    float fast = sin(u_time * 47.3) * 0.5 + sin(u_time * 83.1) * 0.5;
    float medium = sin(u_time * 13.7 + 1.2) * 0.5 + cos(u_time * 19.4) * 0.5;
    float shake = u_audio_peak * 0.012 * (fast * 0.6 + medium * 0.4);
    uv += vec2(shake, shake * 0.71);
    uv = clamp(uv, 0.0, 1.0);

    // 2. CLAW DISTORTION — jagged frequency and depth oscillate fast
    float luma = getLuma(texture2D(u_texture, uv).rgb);
    vec2 stretchUV = uv;
    if (luma < 0.4)
    {
        float freq = 40.0 + 20.0 * sin(u_time * 7.3); // wiggles 20–60 Hz
        float depth = (0.4 - luma) * (0.04 + 0.03 * abs(sin(u_time * 11.1))) * (0.6 + u_audio_rms * 0.8);
        float phase = u_time * 3.7 + hash(floor(u_time * 24.0)) * 6.28; // frame-rate flicker
        stretchUV.y -= depth * sin(uv.x * freq + phase);
        stretchUV.x -= depth * 0.4 * cos(uv.y * freq * 0.7 + phase * 1.3);
    }
    stretchUV = clamp(stretchUV, 0.0, 1.0);

    // 3. EDGE DETECTION — offset and threshold animated rapidly
    float t_fast = u_time * 23.0;
    float offset = 0.002 + 0.003 * abs(sin(t_fast)) + 0.002 * abs(sin(t_fast * 1.618));
    float l = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(-offset, 0.0), 0.0, 1.0)).rgb);
    float r = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(offset, 0.0), 0.0, 1.0)).rgb);
    float t = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(0.0, offset), 0.0, 1.0)).rgb);
    float b = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(0.0, -offset), 0.0, 1.0)).rgb);

    // Diagonal samples add crunchier, more chaotic edges
    float tl = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(-offset, offset), 0.0, 1.0)).rgb);
    float br = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(offset, -offset), 0.0, 1.0)).rgb);

    float edge = abs(l - r) + abs(t - b) + abs(tl - br) * 0.5;

    // Threshold pulses fast — edges flash in and out
    float lo_thresh = 0.08 + 0.06 * sin(u_time * 31.0);
    float hi_thresh = 0.28 + 0.12 * sin(u_time * 17.3 + 1.0);
    edge = smoothstep(lo_thresh, hi_thresh, edge);

    // 4. POSTERIZATION — step count flickers between 3 and 6
    float steps = 3.0 + floor(3.0 * hash(floor(u_time * 12.0)));
    vec3 texColor = texture2D(u_texture, stretchUV).rgb;
    texColor = floor(texColor * steps + 0.5) / steps;

    // 5. HORROR PALETTE — tint colour and mix weight pulse
    float tint_shift = 0.5 + 0.5 * sin(u_time * 5.3);
    vec3 horrorTint = mix(vec3(0.8, 0.1, 0.2), vec3(0.3, 0.05, 0.5), tint_shift);
    float tint_mix = 0.5 + 0.2 * sin(u_time * 8.7);
    vec3 finalColor = mix(texColor, horrorTint * texColor, tint_mix);

    // Ink edges — ink width flickers
    float ink_str = 0.7 + 0.3 * sin(u_time * 29.0);
    finalColor = mix(finalColor, vec3(0.0), edge * ink_str);

    // 6. VIGNETTE — breathes in and out
    float vign_inner = 0.15 + 0.1 * sin(u_time * 3.1);
    float vign_outer = 0.75 + 0.08 * sin(u_time * 4.7 + 1.5);
    float dist = distance(v_texcoord, vec2(0.5, 0.5));
    finalColor *= smoothstep(vign_outer, vign_inner, dist);

    float effectOpacity = clamp(0.30 + edge * 0.40 + u_audio_rms * 0.25, 0.20, 0.72);
    vec3 composed = mix(base.rgb, clamp(finalColor, 0.0, 1.0), effectOpacity);

    gl_FragColor = vec4(composed, base.a);
}

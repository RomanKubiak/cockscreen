precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;

// Helper to get brightness
float getLuma(vec3 color)
{
    return dot(color, vec3(0.299, 0.587, 0.114));
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec2 uv = v_texcoord;

    // 1. JITTER & SHAKE (Sinister Tremor)
    // Low-frequency shake based on music peak
    float shake = (u_audio_peak * 0.01) * sin(u_time * 20.0);
    uv += shake;
    uv = clamp(uv, 0.0, 1.0);

    // 2. THE "CLAW" DISTORTION (Directional Shadow Bleed)
    // We sample the brightness to find "dark" areas
    float luma = getLuma(texture2D(u_texture, uv).rgb);

    // If it's dark, we stretch the UV coordinates downwards/outwards
    // This makes shadows look like they are reaching or "melting"
    vec2 stretchUV = uv;
    if (luma < 0.4)
    {
        float stretchStrength = (0.4 - luma) * 0.05 * u_audio_rms;
        stretchUV.y -= stretchStrength * sin(uv.x * 50.0 + u_time); // Jagged claws
    }
    stretchUV = clamp(stretchUV, 0.0, 1.0);

    // 3. CRUNCHY EDGE DETECTION (The "Sketchy" Outline)
    float offset = 0.003;
    float l = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(-offset, 0.0), 0.0, 1.0)).rgb);
    float r = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(offset, 0.0), 0.0, 1.0)).rgb);
    float t = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(0.0, offset), 0.0, 1.0)).rgb);
    float b = getLuma(texture2D(u_texture, clamp(stretchUV + vec2(0.0, -offset), 0.0, 1.0)).rgb);

    float edge = abs(l - r) + abs(t - b);
    edge = smoothstep(0.1, 0.4, edge); // Make edges sharp and "inked"

    // 4. COLOR POSTERIZATION (Cartoonish/Low-Fi)
    vec3 texColor = texture2D(u_texture, stretchUV).rgb;
    // Snap colors to 4 levels of intensity
    texColor = floor(texColor * 4.0 + 0.5) / 4.0;

    // 5. THE "EVIL" PALETTE SHIFT
    // We darken everything and push it toward deep reds/purples
    vec3 horrorTint = vec3(0.8, 0.2, 0.3); // Blood Red / Sinister Purple tint
    vec3 finalColor = mix(texColor, horrorTint * texColor, 0.6);

    // Apply the black ink edges
    finalColor = mix(finalColor, vec3(0.0, 0.0, 0.0), edge * 0.8);

    // 6. VIGNETTE (Focuses the horror in the center)
    float dist = distance(v_texcoord, vec2(0.5, 0.5));
    finalColor *= smoothstep(0.8, 0.2, dist);

    float effectOpacity = clamp(0.30 + edge * 0.35 + u_audio_rms * 0.20, 0.20, 0.65);
    vec3 composed = mix(base.rgb, clamp(finalColor, 0.0, 1.0), effectOpacity);

    gl_FragColor = vec4(composed, base.a);
}
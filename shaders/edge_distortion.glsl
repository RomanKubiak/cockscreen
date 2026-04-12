precision mediump float;

varying vec2 v_texCoord;
uniform sampler2D u_lastFrame;

// Your music inputs
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform float u_audio_waveform[64];

// HELPER: Small Box Blur (Fast for Pi)
// Returns the average luminosity around the coordinate
float getBlurredLuma(vec2 coord, sampler2D tex, float offset)
{
    vec2 off = vec2(offset); // Offset in UV coordinates (e.g., 0.005)

    // Sample 4 points in a small cross
    float s1 = texture2D(tex, coord + vec2(off.x, 0.0)).r;
    float s2 = texture2D(tex, coord + vec2(-off.x, 0.0)).r;
    float s3 = texture2D(tex, coord + vec2(0.0, off.y)).r;
    float s4 = texture2D(tex, coord + vec2(0.0, -off.y)).r;

    // Average them. We only need the red channel for "lightness"
    return (s1 + s2 + s3 + s4) * 0.25;
}

void main()
{
    vec2 uv = v_texCoord;

    // == 1. INITIAL SETUP ==
    vec2 center = uv - 0.5;

    // == 2. SPATIAL FEEDBACK VORTEX ==
    float bass = u_audio_fft[0] + u_audio_fft[1];
    float highs = u_audio_fft[14] + u_audio_fft[15];

    float zoom = 0.99 - (u_audio_rms * 0.04);
    float rotation = (highs * 0.1) + (u_time * 0.05);
    float s = sin(rotation);
    float c = cos(rotation);
    center *= mat2(c, -s, s, c);
    center *= (zoom - bass * 0.02);

    vec2 sampleUV = center + 0.5;

    // ============================================
    // == NEW: THE "EDGE MELT" DISTORTION PASS ==
    // ============================================

    // a. Determine distortion strength based on audio RMS
    float meltStrength = u_audio_rms * 0.05; // 0.0 to 0.05 offset

    // b. Calculate Gradient (Edge Normal)
    // We sample luminosity at two offset points (small x, small y)
    float blurOffset = 0.005; // Base radius of the edge finder
    float currentLuma = getBlurredLuma(sampleUV, u_lastFrame, blurOffset);
    float deltaX = getBlurredLuma(sampleUV + vec2(0.001, 0.0), u_lastFrame, blurOffset);
    float deltaY = getBlurredLuma(sampleUV + vec2(0.0, 0.001), u_lastFrame, blurOffset);

    // The vector pointing towards brighter areas (normal vector)
    vec2 edgeNormal = vec2(deltaX - currentLuma, deltaY - currentLuma);

    // c. Apply the Melt
    // We offset the main sampleUV based on this normal, weighted by meltStrength
    vec2 meltedUV = sampleUV - (edgeNormal * meltStrength);

    // ============================================

    // == 3. THE FEEDBACK LOOKUP (Using MeltedUV) ==
    // Note: We use the MeltedUV for the aberration offsets too.
    float aberration = 0.002 + (u_audio_peak * 0.01);
    float r = texture2D(u_lastFrame, meltedUV).r;
    float g = texture2D(u_lastFrame, meltedUV - aberration).g;
    float b = texture2D(u_lastFrame, meltedUV + aberration).b;

    vec3 feedback = vec3(r, g, b);

    // == 4. INJECT NEW DRAWING DATA (Using original UV) ==
    int waveIndex = int(uv.x * 63.0);
    float waveValue = u_audio_waveform[waveIndex];
    float spark = smoothstep(0.008, 0.0, abs(uv.y - 0.5 - (waveValue * 0.2)));
    vec3 newSignal = vec3(spark) * vec3(u_audio_peak, u_audio_rms, highs);

    // == 5. FINAL BLEND ==
    float persistence = clamp(0.92 + (u_audio_rms * 0.05), 0.85, 0.98);
    vec3 finalColor = newSignal + (feedback * persistence);

    // Subtle Analog Grain
    float noise = fract(sin(dot(uv + u_time, vec2(12.9898, 78.233))) * 43758.5453);
    finalColor += noise * 0.01;

    gl_FragColor = vec4(finalColor, 1.0);
}
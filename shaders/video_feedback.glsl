precision mediump float;

varying vec2 v_texCoord;
uniform sampler2D u_lastFrame;

// Your specific inputs
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform float u_audio_waveform[64];

void main()
{
    vec2 uv = v_texCoord;

    // 1. DYNAMIC DISTORTION (Using Waveform)
    // We use the waveform to create a "wobble" in the lookup coordinates.
    // Indexing 64 samples: we map UV.x to the array index.
    int waveIndex = int(uv.x * 63.0);
    float waveValue = u_audio_waveform[waveIndex];

    // 2. SPATIAL FEEDBACK (The "Camera" moves based on music)
    vec2 center = uv - 0.5;

    // Use RMS to control the "zoom" pulse
    float zoom = 0.99 - (u_audio_rms * 0.05);

    // Use specific FFT bins to control rotation and skew
    // Low frequencies (Bass) = Scale, High frequencies = Rotation
    float bass = u_audio_fft[0] + u_audio_fft[1];
    float highs = u_audio_fft[14] + u_audio_fft[15];

    float rotation = (highs * 0.1) + (u_time * 0.05); // Continuous slow drift + treble spikes
    float s = sin(rotation);
    float c = cos(rotation);
    center *= mat2(c, -s, s, c);
    center *= (zoom - bass * 0.02);

    // 3. APPLY WAVEFORM "SCANLINE" DISTORTION
    // This mimics electromagnetic interference on an analog signal
    center.y += waveValue * 0.02 * u_audio_rms;

    vec2 sampleUV = center + 0.5;

    // 4. THE FEEDBACK LOOKUP (With Chromatic Aberration)
    // We offset the RGB samples based on the Peak value
    float aberration = 0.002 + (u_audio_peak * 0.01);
    float r = texture2D(u_lastFrame, sampleUV).r;
    float g = texture2D(u_lastFrame, sampleUV - aberration).g;
    float b = texture2D(u_lastFrame, sampleUV + aberration).b;

    vec3 feedback = vec3(r, g, b);

    // 5. INJECT NEW DRAWING DATA
    // We create a "spark" based on the waveform to feed the loop
    float spark = smoothstep(0.01, 0.0, abs(uv.y - 0.5 - (waveValue * 0.2)));
    vec3 newSignal = vec3(spark) * vec3(u_audio_peak, u_audio_rms, highs);

    // 6. FINAL BLEND
    // High RMS makes the feedback persist longer (energy buildup)
    float persistence = clamp(0.92 + (u_audio_rms * 0.05), 0.85, 0.98);
    vec3 finalColor = newSignal + (feedback * persistence);

    // Subtle Analog Grain
    float noise = fract(sin(dot(uv + u_time, vec2(12.9898, 78.233))) * 43758.5453);
    finalColor += noise * 0.02;

    gl_FragColor = vec4(finalColor, 1.0);
}
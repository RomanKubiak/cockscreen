precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform float u_audio_waveform[64];

// ============================================================
// Shape
const float kAmplitudeQuiet = 0.20;  // baseline waveform height    (0.0 – 0.5)
const float kAmplitudeLoud  = 0.32;  // extra height at full volume  (0.0 – 0.5)
const int   kTraceCount     = 3;     // stacked trace copies         (1 – 5)
const float kTraceSpread    = 0.07;  // vertical gap between traces  (0.0 – 0.2)
const float kTraceXLag      = 0.035; // horizontal lag per trace     (0.0 – 0.1)
const float kWidthQuiet     = 0.007; // trace half-thickness at quiet(0.002 – 0.03)
const float kWidthLoud      = 0.012; // extra thickness at full volume

// Glow
const float kGlowRadius     = 0.050; // halo size around each trace  (0.0 – 0.15)
const float kGlowStrength   = 0.60;  // halo intensity               (0.0 – 1.0)

// Color — traces blend from cool (quiet/bass) to warm (loud/treble)
const vec3  kColorCool      = vec3(0.05, 0.88, 1.00);
const vec3  kColorWarm      = vec3(1.00, 0.15, 0.50);
const float kBrightBoost    = 1.25;  // trace brightness multiplier

// CRT scanlines
const float kScanDark       = 0.18;  // darkening depth              (0.0 = off)
const float kScanDensity    = 200.0; // lines per screen height

// Glitch — horizontal block shift
const float kGlitchRate     = 0.55;  // probability per 8 Hz tick    (0.0 – 1.0)
const float kGlitchBlockH   = 0.08;  // block height in UV           (0.02 – 0.2)
const float kGlitchShift    = 0.055; // max horizontal UV shift
const float kGlitchFast     = 0.40;  // secondary 24 Hz glitch probability

// Glitch — signal dropout (a band of x goes to noise)
const float kDropoutRate    = 0.28;  // probability per 3 Hz tick    (0.0 – 1.0)
const float kDropoutMaxLen  = 0.12;  // max dropout band width in UV

// Chromatic aberration
const float kChromaQuiet    = 0.003; // R/B split at quiet
const float kChromaLoud     = 0.014; // extra split at full volume

// Film grain
const float kGrainQuiet     = 0.030; // baseline grain level
const float kGrainLoud      = 0.065; // extra grain at full volume
// ============================================================

float hash1(float n) { return fract(sin(n) * 43758.5453); }
float hash2(vec2 p)  { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }

float sample_wave(float t)
{
    float s = clamp(t, 0.0, 1.0) * 63.0;
    int a = int(s);
    if (a < 0)  a = 0;
    if (a > 63) a = 63;
    int b = a + 1;
    if (b > 63) b = 63;
    return mix(u_audio_waveform[a], u_audio_waveform[b], fract(s));
}

void main()
{
    vec2 uv = v_texcoord;

    // Audio metrics
    float drive  = clamp(u_audio_rms * 2.2 + u_audio_peak * 1.1, 0.0, 1.0);
    float bass   = clamp(u_audio_fft[0] * 1.4 + u_audio_fft[1] * 0.6, 0.0, 1.0);
    float treble = clamp(u_audio_fft[13] + u_audio_fft[14] + u_audio_fft[15], 0.0, 1.0);

    // Time slots for glitch events (independent rates)
    float t8  = floor(u_time * 8.0);
    float t24 = floor(u_time * 24.0);
    float t3  = floor(u_time * 3.0);

    // ---- Block-shift glitch ----------------------------------------
    float gA  = step(1.0 - kGlitchRate, hash1(t8));
    float gB  = step(1.0 - kGlitchFast, hash1(t24 + 0.7)) * drive;
    float row = floor(uv.y / kGlitchBlockH);
    float sh1 = gA * (hash1(row + t8  * 137.3) * 2.0 - 1.0) * kGlitchShift * (0.4 + drive * 0.6);
    float sh2 = gB * (hash1(row + t24 * 91.7)  * 2.0 - 1.0) * kGlitchShift * 0.45;
    float gshift = sh1 + sh2;

    // ---- Signal dropout band ---------------------------------------
    float dOn       = step(1.0 - kDropoutRate, hash1(t3)) * (0.2 + drive * 0.8);
    float dX0       = hash1(t3 + 31.0);
    float dX1       = dX0 + hash1(t3 + 73.0) * kDropoutMaxLen;
    float in_dropout = step(dX0, uv.x) * step(uv.x, dX1) * dOn;

    // ---- Background with chromatic aberration ----------------------
    float chroma = kChromaQuiet + drive * kChromaLoud;
    float sx = uv.x + gshift;
    vec3 bg = vec3(
        texture2D(u_texture, vec2(sx + chroma, uv.y)).r,
        texture2D(u_texture, vec2(sx,           uv.y)).g,
        texture2D(u_texture, vec2(sx - chroma,  uv.y)).b
    );
    float bg_a = texture2D(u_texture, uv).a;

    // ---- Film grain ------------------------------------------------
    float grain = (hash2(uv + fract(vec2(u_time * 7.31, u_time * 3.17))) * 2.0 - 1.0)
                  * (kGrainQuiet + drive * kGrainLoud);
    bg += grain;

    // ---- Waveform traces -------------------------------------------
    float amplitude = kAmplitudeQuiet + drive * kAmplitudeLoud;
    float half_w    = kWidthQuiet     + drive * kWidthLoud;
    float glow_r    = kGlowRadius * (1.0 + drive * 0.5);

    vec3 wave_rgb = vec3(0.0);

    for (int i = 0; i < kTraceCount; i++)
    {
        float fi = float(i);
        float t  = fi / max(float(kTraceCount - 1), 1.0);  // 0..1 across traces

        // Each trace samples with a slight horizontal lag for depth
        float wx = clamp(sx + (t - 0.5) * kTraceXLag, 0.0, 1.0);

        // In the dropout band: replace signal with crackling noise
        float dropout_noise = (hash2(uv + vec2(u_time * 77.0, fi)) * 2.0 - 1.0) * 0.5;
        float w = mix(sample_wave(wx), dropout_noise, in_dropout);

        float offset = (fi - float(kTraceCount - 1) * 0.5) * kTraceSpread;
        float center = 0.5 + offset + w * amplitude;
        float dist   = abs(uv.y - center);

        // Core line + glow halo
        float core = smoothstep(half_w, 0.0, dist);
        float glow = smoothstep(glow_r, half_w, dist) * kGlowStrength;

        // Color: blend cool→warm by trace position and live energy
        float energy_t = clamp(t + treble * 0.45 - bass * 0.15 + drive * 0.25, 0.0, 1.0);
        vec3 tc = mix(kColorCool, kColorWarm, energy_t) * kBrightBoost;

        wave_rgb = max(wave_rgb, tc * (core + glow * 0.55));
    }

    // ---- CRT scanlines (applied uniformly over everything) ---------
    float scan = 1.0 - kScanDark * (0.5 + 0.5 * sin(uv.y * kScanDensity * 3.14159));

    // ---- Composite: additive waveform onto background --------------
    vec3 color = (bg + wave_rgb) * scan;

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), bg_a);
}

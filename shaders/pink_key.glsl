#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform float u_midi_primary;
uniform float u_midi_secondary;
uniform float u_midi_notes[8];
uniform float u_midi_velocities[8];

// HSV chroma key — target color in HSV (0-1 each)
// When all three are zero, defaults to yellow (H≈0.157, backward compat)
uniform float u_key_h;      // hue target   0-1  (0=red, 0.157=yellow, 0.33=green, 0.66=blue)
uniform float u_key_hrange; // hue tolerance half-width   0-0.5  (default 0.08 ≈ ±29°)
uniform float u_key_spread; // edge softness  0-1  (default 0.5: outer edge = 1.5× hrange)
uniform float u_key_smin;   // minimum pixel saturation to key  0-1  (default 0.15, rejects grays)

// Audio-reactive controls.
// u_audio_algorithm selects the detector that drives the key:
// 0 = bass focus (bands 0-3)
// 1 = low-mid focus (bands 4-7)
// 2 = high-mid focus (bands 8-11)
// 3 = high focus (bands 12-15)
// 4 = weighted spectral centroid across all 16 bands
// 5 = full-spectrum energy average
uniform float u_audio_algorithm;
uniform float u_audio_reactivity; // 0-1.5, default 0.45
uniform float u_midi_reactivity;  // 0-1.5, default 0.35

// Convert RGB to HSV. Returns vec3(hue 0-1, saturation 0-1, value 0-1).
vec3 rgb_to_hsv(vec3 c)
{
    float cmax = max(c.r, max(c.g, c.b));
    float cmin = min(c.r, min(c.g, c.b));
    float delta = cmax - cmin;

    float h = 0.0;
    if (delta > 0.001)
    {
        if (cmax == c.r)
        {
            h = mod((c.g - c.b) / delta, 6.0) / 6.0;
        }
        else if (cmax == c.g)
        {
            h = ((c.b - c.r) / delta + 2.0) / 6.0;
        }
        else
        {
            h = ((c.r - c.g) / delta + 4.0) / 6.0;
        }
        if (h < 0.0)
        {
            h += 1.0;
        }
    }
    float s = cmax > 0.001 ? delta / cmax : 0.0;
    return vec3(h, s, cmax);
}

float fft_average(int start_index, int end_index)
{
    float total = 0.0;
    float count = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        if (i < start_index || i > end_index)
        {
            continue;
        }

        total += u_audio_fft[i];
        count += 1.0;
    }

    return count > 0.0 ? total / count : 0.0;
}

float spectral_centroid_drive()
{
    float weighted_sum = 0.0;
    float energy_sum = 0.0;
    for (int i = 0; i < 16; ++i)
    {
        float energy = max(u_audio_fft[i], 0.0);
        weighted_sum += energy * float(i);
        energy_sum += energy;
    }

    if (energy_sum <= 0.0001)
    {
        return 0.0;
    }

    return (weighted_sum / energy_sum) / 15.0;
}

float selected_audio_drive(float algorithm)
{
    if (algorithm < 0.5)
    {
        return fft_average(0, 3);
    }
    if (algorithm < 1.5)
    {
        return fft_average(4, 7);
    }
    if (algorithm < 2.5)
    {
        return fft_average(8, 11);
    }
    if (algorithm < 3.5)
    {
        return fft_average(12, 15);
    }
    if (algorithm < 4.5)
    {
        return spectral_centroid_drive();
    }
    return fft_average(0, 15);
}

float midi_drive()
{
    float note_energy = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        note_energy = max(note_energy, clamp(u_midi_velocities[i], 0.0, 1.0));
    }

    float control_energy = max(clamp(u_midi_primary, 0.0, 1.0), clamp(u_midi_secondary, 0.0, 1.0));
    return max(note_energy, control_energy * 0.85);
}

void main()
{
    vec4 color = texture2D(u_texture, v_texcoord);
    vec3 hsv = rgb_to_hsv(color.rgb);

    // Fall back to yellow when no custom target is set
    float has_custom = step(0.001, u_key_h + u_key_hrange);
    float target_h = mix(0.157, u_key_h, has_custom);
    float hrange = mix(0.08, u_key_hrange, step(0.001, u_key_hrange));
    float spread = mix(0.5, u_key_spread, step(0.001, u_key_spread));
    float smin = mix(0.15, u_key_smin, step(0.001, u_key_smin));

    float audio_amount = mix(0.45, u_audio_reactivity, step(0.001, u_audio_reactivity));
    float midi_amount = mix(0.35, u_midi_reactivity, step(0.001, u_midi_reactivity));
    float audio_drive =
        clamp(selected_audio_drive(u_audio_algorithm) * (1.8 + u_audio_rms * 0.9) + u_audio_peak * 0.25, 0.0, 1.0);
    float midi_energy = clamp(midi_drive(), 0.0, 1.0);

    float hue_shift = audio_drive * audio_amount * 0.09 + midi_energy * midi_amount * 0.06;
    target_h = fract(target_h + hue_shift);
    hrange = clamp(hrange + audio_drive * audio_amount * 0.05 + midi_energy * midi_amount * 0.03, 0.01, 0.5);
    spread = clamp(spread + audio_drive * audio_amount * 0.45 + midi_energy * midi_amount * 0.30, 0.0, 1.0);
    smin = clamp(smin - audio_drive * 0.08 - midi_energy * 0.05, 0.02, 0.95);

    // Circular hue distance (handles red wrap-around at 0/1)
    float hue_diff = abs(hsv.x - target_h);
    hue_diff = min(hue_diff, 1.0 - hue_diff);

    // Hue match: hard edge at hrange, soft falloff to hrange*(1+spread)
    float outer = hrange * (1.0 + spread);
    float hue_match = 1.0 - smoothstep(hrange, outer, hue_diff);

    // Saturation gate: reject pixels that are too gray
    float sat_match = smoothstep(smin - 0.04, smin + 0.04, hsv.y);

    float keyed = hue_match * sat_match;
    float alpha = color.a * (1.0 - keyed);

    gl_FragColor = vec4(color.rgb, alpha);
}

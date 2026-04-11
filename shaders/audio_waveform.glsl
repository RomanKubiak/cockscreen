precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform float u_audio_waveform[64];

float sample_waveform(float t)
{
    float scaled = clamp(t, 0.0, 1.0) * 63.0;
    int left_index = int(floor(scaled));
    int right_index = min(left_index + 1, 63);
    float mix_value = fract(scaled);
    return mix(u_audio_waveform[left_index], u_audio_waveform[right_index], mix_value);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);

    float fft_low = (u_audio_fft[0] + u_audio_fft[1] + u_audio_fft[2] + u_audio_fft[3]) * 0.25;
    float fft_high = (u_audio_fft[12] + u_audio_fft[13] + u_audio_fft[14] + u_audio_fft[15]) * 0.25;
    float amplitude = 0.12 + u_audio_rms * 0.38 + u_audio_peak * 0.18;

    float waveform = sample_waveform(v_texcoord.x);
    float center = 0.5 + waveform * amplitude;
    float distance_to_line = abs(v_texcoord.y - center);

    float line = smoothstep(0.018, 0.0, distance_to_line);
    float glow = smoothstep(0.11 + fft_low * 0.08, 0.0, distance_to_line);
    float scan = 0.75 + 0.25 * sin(v_texcoord.x * 70.0 - u_time * (2.0 + fft_high * 6.0));

    vec3 glow_color = mix(vec3(0.10, 0.85, 0.50), vec3(0.25, 0.90, 1.00), fft_high);
    vec3 line_color = mix(vec3(0.95, 1.00, 0.95), vec3(1.00, 0.95, 0.55), fft_low);

    vec3 color = base.rgb;
    color += glow_color * glow * (0.18 + u_audio_rms * 0.20);
    color = mix(color, line_color * scan, line);

    gl_FragColor = vec4(color, base.a);
}
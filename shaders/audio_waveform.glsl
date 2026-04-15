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
    if (left_index < 0)
    {
        left_index = 0;
    }
    if (left_index > 63)
    {
        left_index = 63;
    }

    int right_index = left_index + 1;
    if (right_index > 63)
    {
        right_index = 63;
    }

    float mix_value = fract(scaled);
    return mix(u_audio_waveform[left_index], u_audio_waveform[right_index], mix_value);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);

    float audio_drive = clamp(u_audio_rms * 2.0 + u_audio_peak * 1.4, 0.0, 1.0);
    float amplitude = 0.12 + audio_drive * 0.30;

    float waveform = sample_waveform(v_texcoord.x);
    float center = 0.5 + waveform * amplitude;
    float distance_to_line = abs(v_texcoord.y - center);

    float line = smoothstep(0.016 + audio_drive * 0.010, 0.0, distance_to_line);

    vec3 color = base.rgb;
    color = mix(color, vec3(1.0), line);

    gl_FragColor = vec4(color, base.a);
}

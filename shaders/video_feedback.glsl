#ifdef GL_ES
precision mediump float;
#endif

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
    vec2 uv = v_texcoord;
    vec4 base = texture2D(u_texture, uv);
    float wave_value = sample_waveform(uv.x);

    float bass = (u_audio_fft[0] + u_audio_fft[1]) * 0.5;
    float highs = (u_audio_fft[14] + u_audio_fft[15]) * 0.5;

    vec2 centered = uv - vec2(0.5);
    float zoom = 0.992 - (u_audio_rms * 0.035);
    float rotation = (highs * 0.08) + (u_time * 0.04);
    float s = sin(rotation);
    float c = cos(rotation);
    centered *= mat2(c, -s, s, c);
    centered *= max(zoom - bass * 0.015, 0.90);
    centered.y += wave_value * (0.010 + u_audio_rms * 0.012);

    vec2 sample_uv = clamp(centered + vec2(0.5), 0.0, 1.0);
    float aberration = 0.0015 + (u_audio_peak * 0.006);
    float r = texture2D(u_texture, clamp(sample_uv + vec2(aberration, 0.0), 0.0, 1.0)).r;
    float g = texture2D(u_texture, sample_uv).g;
    float b = texture2D(u_texture, clamp(sample_uv - vec2(aberration, 0.0), 0.0, 1.0)).b;
    vec3 feedback = vec3(r, g, b);

    float spark = smoothstep(0.018, 0.0, abs(uv.y - 0.5 - wave_value * 0.18));
    vec3 spark_color = mix(vec3(0.10, 0.75, 1.0), vec3(1.0, 0.35, 0.22), highs);
    vec3 injected = spark_color * spark * (0.10 + u_audio_peak * 0.35);

    float persistence = clamp(0.78 + (u_audio_rms * 0.14), 0.76, 0.92);
    vec3 color = mix(base.rgb, feedback, 0.32 + u_audio_rms * 0.18);
    color = mix(color, feedback * persistence + injected, 0.55 + bass * 0.12);

    float noise = fract(sin(dot(uv + u_time, vec2(12.9898, 78.233))) * 43758.5453);
    color += (noise - 0.5) * 0.015;

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
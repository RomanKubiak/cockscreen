#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];

void main()
{
    vec2 uv = v_texcoord;
    vec4 base = texture2D(u_texture, uv);

    float bass = (u_audio_fft[0] + u_audio_fft[1]) * 0.5;
    float highs = (u_audio_fft[14] + u_audio_fft[15]) * 0.5;

    vec2 centered = uv - vec2(0.5);
    float zoom = 0.994 - (u_audio_rms * 0.030);
    float rotation = (highs * 0.08) + (u_time * 0.04);
    float s = sin(rotation);
    float c = cos(rotation);
    centered *= mat2(c, -s, s, c);
    centered *= max(zoom - bass * 0.015, 0.90);

    float wobble = sin(uv.x * 80.0 + u_time * (4.0 + highs * 6.0));
    centered.y += wobble * (0.002 + u_audio_rms * 0.010);

    vec2 sample_uv = clamp(centered + vec2(0.5), 0.0, 1.0);
    float aberration = 0.0012 + (u_audio_peak * 0.004);
    float r = texture2D(u_texture, clamp(sample_uv + vec2(aberration, 0.0), 0.0, 1.0)).r;
    float g = texture2D(u_texture, sample_uv).g;
    float b = texture2D(u_texture, clamp(sample_uv - vec2(aberration, 0.0), 0.0, 1.0)).b;
    vec3 feedback = vec3(r, g, b);

    float line = sin(uv.x * 42.0 + u_time * (2.2 + bass * 5.0));
    float spark = smoothstep(0.020, 0.0, abs(uv.y - 0.5 - line * (0.03 + u_audio_rms * 0.08)));
    vec3 spark_color = mix(vec3(0.10, 0.75, 1.0), vec3(1.0, 0.35, 0.22), highs);
    vec3 injected = spark_color * spark * (0.08 + u_audio_peak * 0.28);

    float persistence = clamp(0.80 + (u_audio_rms * 0.10), 0.78, 0.90);
    vec3 color = mix(base.rgb, feedback, 0.26 + u_audio_rms * 0.16);
    color = mix(color, feedback * persistence + injected, 0.48 + bass * 0.10);

    float noise = fract(sin(dot(uv + u_time, vec2(12.9898, 78.233))) * 43758.5453);
    color += (noise - 0.5) * 0.010;

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
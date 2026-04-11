precision mediump float;

varying vec2 v_texcoord;
uniform vec2 u_viewport_size;
uniform float u_time;
uniform float u_audio_level;
uniform float u_audio_fft[16];
uniform sampler2D u_texture;

float edge(vec2 p, vec2 a, vec2 b)
{
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec2 p = v_texcoord * 2.0 - 1.0;
    p.x *= u_viewport_size.x / max(u_viewport_size.y, 1.0);

    float low = (u_audio_fft[0] + u_audio_fft[1] + u_audio_fft[2] + u_audio_fft[3]) * 0.25;
    float low_mid = (u_audio_fft[4] + u_audio_fft[5] + u_audio_fft[6] + u_audio_fft[7]) * 0.25;
    float high_mid = (u_audio_fft[8] + u_audio_fft[9] + u_audio_fft[10] + u_audio_fft[11]) * 0.25;
    float high = (u_audio_fft[12] + u_audio_fft[13] + u_audio_fft[14] + u_audio_fft[15]) * 0.25;
    float motion = clamp(low * 0.9 + low_mid * 0.75 + high_mid * 0.55 + high * 0.35, 0.0, 1.0);

    float angle = u_time * (0.75 + low * 1.8 + high * 0.4);
    mat2 rotation = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
    p = rotation * p;

    p *= 0.76 + low * 0.26;

    vec2 a = vec2(0.0, 0.94);
    vec2 b = vec2(-0.88, -0.58);
    vec2 c = vec2(0.88, -0.58);

    float s1 = edge(p, a, b);
    float s2 = edge(p, b, c);
    float s3 = edge(p, c, a);
    float winding = sign(edge(a, b, c));
    float inside = step(0.0, min(min(s1 * winding, s2 * winding), s3 * winding));
    float edge_distance = min(min(abs(s1), abs(s2)), abs(s3));
    float edge_band = 1.0 - smoothstep(0.0, 0.045, edge_distance);

    float pulse = 0.5 + 0.5 * sin(u_time * (2.0 + motion * 3.5));
    vec3 fill_color = mix(vec3(0.12, 0.72, 1.0), vec3(1.0, 0.38, 0.18), clamp(low_mid * 1.5 + pulse * 0.35, 0.0, 1.0));
    vec3 outline_color = mix(vec3(1.0, 1.0, 1.0), vec3(1.0, 0.84, 0.35), high);

    vec3 background = base.rgb * (0.12 + 0.18 * low) + vec3(0.02, 0.02, 0.04) * high;
    vec3 color = mix(background, outline_color, edge_band);
    color = mix(color, fill_color, inside);
    color += inside * (0.10 + 0.18 * motion);
    color += edge_band * 0.10 * low_mid;

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}

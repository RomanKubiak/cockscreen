precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_level;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform float u_audio_waveform[64];
uniform vec2 u_resolution;

mat2 rot(float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c);
}

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

float noise(vec2 p)
{
    vec2 cell = floor(p);
    vec2 local = fract(p);
    vec2 smooth = local * local * (3.0 - 2.0 * local);

    float a = hash(cell);
    float b = hash(cell + vec2(1.0, 0.0));
    float c = hash(cell + vec2(0.0, 1.0));
    float d = hash(cell + vec2(1.0, 1.0));

    return mix(mix(a, b, smooth.x), mix(c, d, smooth.x), smooth.y);
}

float fbm(vec2 p)
{
    float value = 0.0;
    float amplitude = 0.5;
    for (int octave = 0; octave < 6; ++octave)
    {
        value += noise(p) * amplitude;
        p = p * 2.02 + vec2(6.4, 3.7);
        amplitude *= 0.5;
    }
    return value;
}

float edge_billow(vec2 p, float radius, float speed, float scale, float seed)
{
    vec2 outward = radius > 0.0001 ? p / radius : vec2(0.0, 1.0);
    vec2 domain = outward * (radius * scale - u_time * speed);

    float coarse = fbm(domain * 1.10 + vec2(seed, -seed * 0.7));
    float detail = fbm(domain * 2.40 + vec2(-seed * 1.7, seed * 1.3));
    float billow = smoothstep(0.42, 0.74, coarse * 0.72 + detail * 0.28);
    billow *= smoothstep(0.22, 1.35, radius);
    return billow;
}

vec3 render_smoke(vec2 uv)
{
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    vec2 p = (uv - 0.5) * vec2(aspect, 1.0) * 2.0;
    float radius = length(p);

    float rms = clamp(u_audio_rms * 4.0, 0.0, 1.0);
    float peak = clamp(u_audio_peak * 3.0, 0.0, 1.0);
    float audio_drive = clamp(rms * 0.75 + peak * 0.50, 0.0, 1.0);
    float burst = smoothstep(0.10, 0.50, peak);

    float base_speed = mix(0.018, 0.070, audio_drive);
    float edge_speed = mix(0.08, 0.38, burst);
    float flow_strength = mix(0.60, 1.55, audio_drive);
    float opacity = mix(0.26, 0.82, audio_drive);
    float final_gain = mix(0.60, 1.35, audio_drive) + burst * 0.20;

    vec2 drift_a = p * rot(0.18 + sin(u_time * 0.05) * 0.07);
    vec2 drift_b = p * rot(-0.24 + cos(u_time * 0.04) * 0.09);

    vec2 flow =
        vec2(fbm(drift_a * 0.80 + vec2(u_time * (0.030 + base_speed), -u_time * (0.020 + base_speed * 0.50))),
             fbm(drift_b * 0.76 + vec2(-u_time * (0.025 + base_speed * 0.70), u_time * (0.018 + base_speed * 0.60))));
    flow = (flow - 0.5) * flow_strength;

    vec2 warped = p + flow;
    float large = fbm(warped * mix(0.80, 0.65, burst) +
                      vec2(u_time * (0.028 + base_speed * 0.40), -u_time * (0.018 + base_speed * 0.30)));
    float medium =
        fbm(warped * mix(1.45, 1.15, burst) +
            vec2(-u_time * (0.038 + base_speed * 0.55), u_time * (0.025 + base_speed * 0.40)) + vec2(3.2, 1.7));
    float detail =
        fbm(warped * mix(2.75, 2.10, burst) +
            vec2(u_time * (0.052 + base_speed * 0.70), u_time * (0.034 + base_speed * 0.55)) + vec2(-4.5, 2.8));

    float body = smoothstep(0.38, 0.66, large * 0.60 + medium * 0.28 + detail * 0.12);
    float wisps = smoothstep(0.46, 0.74, medium * 0.52 + detail * 0.48);
    float haze = smoothstep(0.22, 0.72, large) * 0.28;

    float edge_a = edge_billow(p, radius, 0.16 + edge_speed, mix(0.95, 0.55, burst), 2.3);
    float edge_b = edge_billow(p, radius, 0.10 + edge_speed * 0.75, mix(1.10, 0.62, burst), 4.7);
    float edge = max(edge_a, edge_b) * (0.08 + burst * 1.10);

    float center_weight = 1.0 - smoothstep(1.00, 1.70, radius);
    float smoke = body * 0.72 + wisps * 0.38 + haze + edge;
    smoke *= 0.78 + center_weight * 0.22;

    vec3 color = vec3(0.10) * haze;
    color += vec3(0.78) * body;
    color += vec3(0.50) * wisps;
    color += vec3(1.0) * edge * (0.30 + burst * 0.65);
    color += vec3(1.0) * smoke * smoke * 0.34;
    return clamp(color * (opacity + final_gain), 0.0, 1.0);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec3 effect = render_smoke(v_texcoord);
    vec3 composed = max(base.rgb, effect);
    gl_FragColor = vec4(composed, 1.0);
}

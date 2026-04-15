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

const float kTau = 6.28318530718;

mat2 rot(float angle)
{
    float s = sin(angle);
    float c = cos(angle);
    return mat2(c, -s, s, c);
}

vec3 rotate_xyz(vec3 p, vec3 angles)
{
    p.yz *= rot(angles.x);
    p.xz *= rot(angles.y);
    p.xy *= rot(angles.z);
    return p;
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
    for (int octave = 0; octave < 5; ++octave)
    {
        value += noise(p) * amplitude;
        p = p * 2.0 + vec2(7.3, 3.1);
        amplitude *= 0.5;
    }
    return value;
}

// lasers originating from a central point
float laser(vec2 p, int num)
{
    float r = atan(p.x, p.y);
    float sn = sin(r * float(num) + u_time);
    float lzr = 0.5 + 0.5 * sn;
    lzr = lzr * lzr * lzr * lzr * lzr;
    float glow = pow(clamp(sn, 0.0, 1.0), 100.0);
    return lzr + glow;
}

float line_axis(float coordinate, float spacing, float width)
{
    float cell = abs(fract(coordinate / spacing) - 0.5) * spacing;
    float core = 1.0 - smoothstep(width * 0.20, width * 0.82, cell);
    float inner_glow = 1.0 - smoothstep(width * 0.85, width * 2.8, cell);
    float outer_glow = 1.0 - smoothstep(width * 2.6, width * 8.0, cell);
    return clamp(core * 1.25 + inner_glow * 0.22 + outer_glow * 0.08, 0.0, 1.0);
}

float rect_mask(vec2 p, vec2 half_extent, float feather)
{
    vec2 edge = abs(p) - half_extent;
    float outside = max(edge.x, edge.y);
    return 1.0 - smoothstep(0.0, feather, outside);
}

float plane_grid_energy(vec3 ro, vec3 rd, vec3 plane_origin, vec3 plane_normal, float grid_angle, float spacing,
                        float width, vec2 half_extent)
{
    float denom = dot(rd, plane_normal);
    if (abs(denom) < 0.0001)
    {
        return 0.0;
    }

    float t = dot(plane_origin - ro, plane_normal) / denom;
    if (t <= 0.0)
    {
        return 0.0;
    }

    vec3 hit = ro + rd * t;
    vec3 up = abs(plane_normal.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 axis_u = normalize(cross(up, plane_normal));
    vec3 axis_v = normalize(cross(plane_normal, axis_u));
    vec2 local = vec2(dot(hit - plane_origin, axis_u), dot(hit - plane_origin, axis_v));
    local *= rot(grid_angle);

    float bounds = rect_mask(local, half_extent, 2.0);
    float gx = line_axis(local.x, spacing, width);
    float gy = line_axis(local.y, spacing, width);
    float grid = max(gx, gy);
    float distance_gain = 1.0 / (1.0 + 0.015 * t * t);
    return grid * bounds * distance_gain;
}

vec3 render_laser_space(vec2 uv)
{
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    vec2 screen = (uv - 0.5) * vec2(aspect, 1.0) * 2.0;

    vec3 ro = vec3(0.0, 0.0, -4.0);
    vec3 rd = normalize(vec3(screen, 1.8));
    vec3 space_angles = vec3(sin(u_time * 0.10) * 0.18, cos(u_time * 0.08) * 0.14, sin(u_time * 0.06) * 0.12);
    vec3 space_offset = vec3(sin(u_time * 0.07) * 0.55, cos(u_time * 0.06) * 0.35, sin(u_time * 0.05) * 0.75);
    rd = rotate_xyz(rd, space_angles);
    ro = rotate_xyz(ro, space_angles) + space_offset;

    float spacing = 0.92;
    float line_width = 0.0055;
    vec2 half_extent = vec2(30.0, 20.0);

    vec3 origin_a = rotate_xyz(vec3(0.9, 0.2, 2.0), vec3(u_time * 0.16, u_time * 0.11, u_time * 0.08));
    origin_a += vec3(sin(u_time * 0.18) * 0.35, cos(u_time * 0.13) * 0.25, sin(u_time * 0.12) * 0.45);
    vec3 normal_a = normalize(rotate_xyz(vec3(0.12, 0.02, -1.0), vec3(u_time * 0.09, u_time * 0.07, u_time * 0.05)));

    vec3 origin_b = rotate_xyz(vec3(-0.8, -0.1, 3.4), vec3(-u_time * 0.12, u_time * 0.15, -u_time * 0.07));
    origin_b += vec3(cos(u_time * 0.11 + 1.2) * 0.32, sin(u_time * 0.17 + 0.6) * 0.28, cos(u_time * 0.10) * 0.40);
    vec3 normal_b = normalize(rotate_xyz(vec3(-0.28, 0.04, -1.0), vec3(-u_time * 0.06, u_time * 0.10, u_time * 0.08)));

    vec3 origin_c = rotate_xyz(vec3(0.2, 0.7, 4.8), vec3(u_time * 0.14, -u_time * 0.09, u_time * 0.12));
    origin_c += vec3(sin(u_time * 0.14 + 2.1) * 0.30, cos(u_time * 0.09 + 0.4) * 0.35, sin(u_time * 0.13 + 0.5) * 0.38);
    vec3 normal_c = normalize(rotate_xyz(vec3(0.0, 0.22, -1.0), vec3(u_time * 0.11, -u_time * 0.07, u_time * 0.09)));

    vec3 origin_d = rotate_xyz(vec3(-0.3, -0.6, 6.2), vec3(-u_time * 0.15, -u_time * 0.10, u_time * 0.06));
    origin_d += vec3(cos(u_time * 0.20 + 0.8) * 0.36, sin(u_time * 0.15 + 1.7) * 0.24, cos(u_time * 0.16 + 0.9) * 0.42);
    vec3 normal_d = normalize(rotate_xyz(vec3(0.24, -0.16, -1.0), vec3(-u_time * 0.08, -u_time * 0.09, u_time * 0.10)));

    float grid_a = plane_grid_energy(ro, rd, origin_a + space_offset, normal_a, space_angles.z + u_time * 0.11, spacing,
                                     line_width, half_extent);
    float grid_b = plane_grid_energy(ro, rd, origin_b + space_offset, normal_b, space_angles.z - u_time * 0.09 + 0.55,
                                     spacing, line_width, half_extent);
    float grid_c = plane_grid_energy(ro, rd, origin_c + space_offset, normal_c, space_angles.y + u_time * 0.07 + 1.10,
                                     spacing, line_width, half_extent);
    float grid_d = plane_grid_energy(ro, rd, origin_d + space_offset, normal_d, space_angles.x - u_time * 0.13 + 0.28,
                                     spacing, line_width, half_extent);

    float beam_energy = max(grid_a, max(grid_b, max(grid_c, grid_d)));
    float total_energy = grid_a + grid_b + grid_c + grid_d;
    float crossing = clamp(total_energy - beam_energy, 0.0, 3.0);

    float smoke = fbm(screen * 1.8 + vec2(u_time * 0.015, -u_time * 0.012));
    smoke = smoothstep(0.62, 1.10, smoke);
    float beam_fog = smoothstep(0.02, 0.18, beam_energy) * (0.01 + smoke * 0.08);
    float beam_glow = smoothstep(0.006, 0.065, beam_energy);
    float beam_mid = smoothstep(0.05, 0.22, beam_energy);
    float beam_core = smoothstep(0.58, 0.98, beam_energy);
    float beam_hot = pow(clamp((beam_energy - 0.72) / 0.28, 0.0, 1.0), 2.2);

    vec3 fog_color = vec3(0.0015, 0.0, 0.0) * smoke;
    vec3 beam_fog_color = vec3(0.040, 0.007, 0.006) * beam_fog;
    vec3 beam_color = vec3(1.0, 0.14, 0.10) * beam_glow * 0.22;
    beam_color += vec3(1.0, 0.05, 0.03) * beam_mid * 0.55;
    beam_color += vec3(1.0, 0.98, 0.92) * beam_core * 1.15;
    beam_color += vec3(1.0, 1.0, 1.0) * beam_hot * 0.95;
    beam_color += vec3(1.0, 0.38, 0.24) * crossing * crossing * 0.14;

    vec3 color = fog_color + beam_fog_color + beam_color + vec3(0.003, 0.0, 0.0);

    return clamp(pow(color, vec3(0.82)) * 1.8, 0.0, 1.0);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec3 effect = render_laser_space(v_texcoord);
    vec3 composed = max(base.rgb, effect);
    gl_FragColor = vec4(composed, base.a);
}
precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform vec2 u_video_size;
uniform float u_time;
uniform float u_wire_density;
uniform float u_audio_fft[16];

const float kPi = 3.14159265359;
const float kTau = 6.28318530718;

float line_mask(float value, float width)
{
    return 1.0 - smoothstep(0.0, width, abs(value));
}

float mirrored_bounce(float t, float min_value, float max_value)
{
    float range = max(max_value - min_value, 0.0001);
    float cycle = mod(t, range * 2.0);
    float mirrored = cycle < range ? cycle : 2.0 * range - cycle;
    return min_value + mirrored;
}

float fft_band(float band_index)
{
    int index = int(clamp(floor(band_index + 0.5), 0.0, 15.0));

    if (index == 0)
        return u_audio_fft[0];
    if (index == 1)
        return u_audio_fft[1];
    if (index == 2)
        return u_audio_fft[2];
    if (index == 3)
        return u_audio_fft[3];
    if (index == 4)
        return u_audio_fft[4];
    if (index == 5)
        return u_audio_fft[5];
    if (index == 6)
        return u_audio_fft[6];
    if (index == 7)
        return u_audio_fft[7];
    if (index == 8)
        return u_audio_fft[8];
    if (index == 9)
        return u_audio_fft[9];
    if (index == 10)
        return u_audio_fft[10];
    if (index == 11)
        return u_audio_fft[11];
    if (index == 12)
        return u_audio_fft[12];
    if (index == 13)
        return u_audio_fft[13];
    if (index == 14)
        return u_audio_fft[14];
    return u_audio_fft[15];
}

vec3 fft_band_color(float band_index)
{
    float t = clamp(band_index / 15.0, 0.0, 1.0);
    vec3 low = vec3(1.0, 0.24, 0.16);
    vec3 low_mid = vec3(1.0, 0.78, 0.18);
    vec3 high_mid = vec3(0.22, 0.95, 0.55);
    vec3 high = vec3(0.18, 0.56, 1.0);

    if (t < 0.33)
    {
        return mix(low, low_mid, t / 0.33);
    }
    if (t < 0.66)
    {
        return mix(low_mid, high_mid, (t - 0.33) / 0.33);
    }

    return mix(high_mid, high, (t - 0.66) / 0.34);
}

vec3 reactive_segment_color(float band_index, float band_level)
{
    float stepped_shift = floor(band_level * 8.0 + 0.5);
    float animated_shift = sin(u_time * (0.7 + band_index * 0.09) + band_level * 10.0) * band_level * 2.0;
    vec3 idle_color = mix(vec3(0.03, 0.03, 0.04), fft_band_color(band_index), 0.12);
    vec3 live_color = fft_band_color(mod(band_index + stepped_shift + animated_shift, 16.0));
    vec3 hot_color = mix(live_color, vec3(1.0), smoothstep(0.68, 1.0, band_level) * 0.35);
    return mix(idle_color, hot_color, band_level);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);

    float radius = 0.24 + 0.02 * sin(u_time * 0.7);
    vec2 min_center = vec2(radius * 1.15);
    vec2 max_center = vec2(1.0 - radius * 1.15);
    vec2 center = vec2(mirrored_bounce(u_time * 0.23 + 0.11, min_center.x, max_center.x),
                       mirrored_bounce(u_time * 0.31 + 0.37, min_center.y, max_center.y));
    vec2 aspect = vec2(max(u_video_size.x, 1.0) / max(u_video_size.y, 1.0), 1.0);
    vec2 p = (v_texcoord - center) * aspect / radius;
    float r2 = dot(p, p);

    if (r2 > 1.0)
    {
        gl_FragColor = base;
        return;
    }

    float z = sqrt(max(0.0, 1.0 - r2));
    vec3 n = normalize(vec3(p, z));

    float spin = u_time * 1.45;
    mat2 twist = mat2(cos(spin), -sin(spin), sin(spin), cos(spin));
    vec2 surface = twist * vec2(atan(n.x, n.z), asin(clamp(n.y, -1.0, 1.0)));

    float density = clamp(u_wire_density, 0.0, 1.0);
    float meridians = floor(mix(6.0, 18.0, density) + 0.5);
    float parallels = floor(mix(4.0, 12.0, density) + 0.5);
    float line_width = mix(0.06, 0.025, density);

    float meridian_mask = line_mask(sin(surface.x * meridians), line_width);
    float parallel_mask = line_mask(sin(surface.y * parallels), line_width);

    float meridian_coord = fract((surface.x + kPi) / kTau) * meridians;
    float parallel_coord = clamp((surface.y + 0.5 * kPi) / kPi, 0.0, 0.9999) * parallels;
    float meridian_cell_index = mod(floor(meridian_coord), max(meridians, 1.0));
    float parallel_cell_index = clamp(floor(parallel_coord), 0.0, max(parallels - 1.0, 0.0));

    float segment_band_index = mod(meridian_cell_index + parallel_cell_index * max(meridians, 1.0), 16.0);
    float segment_band_level = pow(clamp(fft_band(segment_band_index) * 4.5, 0.0, 1.0), 0.18);

    float rim_fade = 1.0 - smoothstep(0.78, 1.0, sqrt(r2));
    float sphere_light = 0.18 + 0.82 * smoothstep(0.0, 1.0, z);
    float grid_mask = max(meridian_mask, parallel_mask);

    vec3 segment_fill = reactive_segment_color(segment_band_index, segment_band_level);
    segment_fill *= rim_fade * (0.18 + sphere_light * 0.55 + segment_band_level * 2.4);

    vec3 grid_color = mix(vec3(0.01, 0.01, 0.02), vec3(0.95, 0.95, 1.0), 0.08 + segment_band_level * 0.22);
    float grid_strength = clamp(grid_mask * (0.7 + segment_band_level * 0.9), 0.0, 1.0);

    vec3 color = mix(segment_fill, grid_color, grid_strength);

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
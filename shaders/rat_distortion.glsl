precision mediump float;

varying vec2 v_texcoord;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;
uniform float u_rat_tooth_count;
uniform float u_rat_tooth_height_min;
uniform float u_rat_tooth_height_max;
uniform float u_rat_tooth_base_min;
uniform float u_rat_tooth_base_max;
uniform float u_rat_tooth_animation_seconds;
uniform float u_rat_tooth_fade_in_seconds;
uniform float u_rat_tooth_fade_out_seconds;
uniform float u_rat_tooth_fade_start_opacity;
uniform float u_rat_tooth_fade_end_opacity;

float hash11(float value)
{
    return fract(sin(value * 127.1) * 43758.5453123);
}

float hash21(vec2 value)
{
    return fract(sin(dot(value, vec2(127.1, 311.7))) * 43758.5453123);
}

float luminance(vec3 color)
{
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

float red_dominance(vec3 color)
{
    return clamp(color.r - max(color.g, color.b) * 1.15, 0.0, 1.0);
}

float triangle_shape(vec2 point, float height, float half_width)
{
    float y = point.y / max(height, 0.0001);
    float width_at_y = mix(half_width, 0.0, clamp(y, 0.0, 1.0));
    float inside = step(0.0, point.y) * step(point.y, height) * step(abs(point.x), width_at_y);
    return inside;
}

float saw_tooth_shape(vec2 point, float height, float base_width, float lean)
{
    vec2 tooth_point = point;
    tooth_point.x += lean * tooth_point.y;
    return triangle_shape(tooth_point, height, base_width);
}

float saw_blade_tooth_shape(vec2 point, float height, float base_width, float lean, float skew, float tip_shift,
                            float jaggedness, float seed, float cycle)
{
    vec2 tooth_point = point;
    tooth_point.x += lean * tooth_point.y;

    float y = clamp(tooth_point.y / max(height, 0.0001), 0.0, 1.0);
    float taper = smoothstep(0.0, 1.0, y);
    float contour_a = sin(y * 10.0 + seed * 6.28318530718 + cycle * 6.0);
    float contour_b = sin(y * 27.0 + seed * 13.0 - cycle * 3.25);
    float contour = (contour_a * 0.18 + contour_b * 0.08) * jaggedness;
    float center = mix(0.0, tip_shift, taper) + contour * base_width;
    float trailing_width = mix(base_width * (1.0 + skew), 0.0, taper) * (1.0 + contour * 0.55);
    float leading_width = mix(base_width * max(0.18, 1.0 - skew * 0.82), 0.0, taper) * (1.0 - contour * 0.35);

    float inside = step(0.0, tooth_point.y) * step(tooth_point.y, height) *
                   step(center - trailing_width, tooth_point.x) * step(tooth_point.x, center + leading_width);

    float crumble_window = smoothstep(0.10, 0.92, y) * smoothstep(0.24, 0.98, cycle);
    float crumble_noise = hash11(floor(y * mix(4.0, 11.0, jaggedness) + seed * 17.0 + cycle * 7.0));
    float crumble = step(0.64, crumble_noise) * crumble_window * jaggedness * 0.72;
    inside *= 1.0 - crumble;

    return inside;
}

float tooth_cycle_envelope(float cycle, float fade_in_seconds, float fade_out_seconds, float animation_seconds,
                           float start_opacity, float end_opacity, float seed, float jaggedness)
{
    float fade_in_window = clamp(fade_in_seconds / max(animation_seconds, 0.001), 0.001, 0.5);
    float fade_out_window = clamp(fade_out_seconds / max(animation_seconds, 0.001), 0.001, 0.5);
    float fade_in_jitter = mix(0.0, fade_in_window * 0.55, hash11(seed + 71.3) * jaggedness);
    float fade_out_jitter = mix(0.0, fade_out_window * 0.55, hash11(seed + 82.1) * jaggedness);

    float fade_in = smoothstep(fade_in_jitter, fade_in_jitter + fade_in_window, cycle);
    float fade_out = 1.0 - smoothstep(1.0 - fade_out_window - fade_out_jitter, 1.0 - fade_out_jitter, cycle);
    float opacity = mix(clamp(start_opacity, 0.0, 1.0), clamp(end_opacity, 0.0, 1.0), fade_in);
    return fade_in * fade_out * opacity;
}

float thorn_shape(vec2 point, float height, float base_width, float lean, float barb_shift)
{
    vec2 thorn_point = point;
    thorn_point.x += lean * thorn_point.y;

    float tip = triangle_shape(thorn_point, height, base_width);
    float stem = step(abs(thorn_point.x + thorn_point.y * lean * 0.18), base_width * 0.10) * step(0.0, thorn_point.y) *
                 step(thorn_point.y, height * 0.88);
    vec2 barb_point = thorn_point - vec2(barb_shift, height * 0.26);
    float barb = triangle_shape(barb_point, height * 0.34, base_width * 0.22);
    return max(tip, max(stem, barb));
}

mat2 rotate2(float angle)
{
    float sine = sin(angle);
    float cosine = cos(angle);
    return mat2(cosine, -sine, sine, cosine);
}

void main()
{
    vec2 uv = v_texcoord;
    vec2 texel = 1.0 / max(u_resolution, vec2(1.0));
    vec4 base = texture2D(u_texture, uv);

    vec3 sample_right = texture2D(u_texture, clamp(uv + vec2(texel.x, 0.0), 0.0, 1.0)).rgb;
    vec3 sample_left = texture2D(u_texture, clamp(uv - vec2(texel.x, 0.0), 0.0, 1.0)).rgb;
    vec3 sample_up = texture2D(u_texture, clamp(uv + vec2(0.0, texel.y), 0.0, 1.0)).rgb;
    vec3 sample_down = texture2D(u_texture, clamp(uv - vec2(0.0, texel.y), 0.0, 1.0)).rgb;

    float luma_right = luminance(sample_right);
    float luma_left = luminance(sample_left);
    float luma_up = luminance(sample_up);
    float luma_down = luminance(sample_down);

    vec2 luma_gradient = vec2(luma_right - luma_left, luma_up - luma_down);
    vec3 color_dx = sample_right - sample_left;
    vec3 color_dy = sample_up - sample_down;
    float red_base = red_dominance(base.rgb);
    float red_right = red_dominance(sample_right);
    float red_left = red_dominance(sample_left);
    float red_up = red_dominance(sample_up);
    float red_down = red_dominance(sample_down);
    float red_focus = max(max(red_base, red_right), max(max(red_left, red_up), red_down));
    red_focus = smoothstep(0.08, 0.55, red_focus);
    vec2 red_gradient = vec2(sample_right.r - sample_left.r, sample_up.r - sample_down.r);
    float color_edge = length(color_dx) + length(color_dy);
    float edge_strength = smoothstep(0.04, 0.32, length(luma_gradient) + color_edge * 0.45) * red_focus;
    vec2 edge_direction = normalize(mix(luma_gradient, red_gradient, 0.8) +
                                    vec2(color_dx.r - color_dx.b, color_dy.r - color_dy.b) * 0.18 + vec2(0.0001));
    vec2 centered_px = vec2((uv.x - 0.5) * u_resolution.x, (uv.y - 0.5) * u_resolution.y);
    float radius_px = length(centered_px);
    vec2 radial_direction = centered_px / max(radius_px, 1.0);
    vec2 tangent = vec2(-radial_direction.y, radial_direction.x);
    float angle = atan(centered_px.y, centered_px.x);

    float angle_seed = hash11(floor((angle + 3.14159265359) / 6.28318530718 * 24.0) + 17.1);
    vec2 distortion = radial_direction * (0.12 + 0.30 * angle_seed) *
                      (0.45 + 0.55 * sin(u_time * 1.9 + angle * 5.0 + angle_seed * 6.28318530718)) * texel * 1.8;
    vec2 distorted_uv = clamp(uv - distortion, 0.0, 1.0);

    float chroma = 0.0;
    vec3 color = vec3(texture2D(u_texture, clamp(distorted_uv + vec2(chroma, 0.0), 0.0, 1.0)).r,
                      texture2D(u_texture, distorted_uv).g,
                      texture2D(u_texture, clamp(distorted_uv - vec2(chroma, 0.0), 0.0, 1.0)).b);

    vec3 edge_average = 0.25 * (sample_right + sample_left + sample_up + sample_down);
    vec3 contrast_color = abs(edge_average - base.rgb);
    float red_energy =
        clamp(contrast_color.r * 1.35 + red_focus * 0.55 - (contrast_color.g + contrast_color.b) * 0.45, 0.0, 1.0);
    float ember_energy = clamp(red_energy * 0.9 + edge_strength * 0.75 + red_base * 0.45, 0.0, 1.0);
    vec3 deep_red = vec3(0.24, 0.01, 0.02);
    vec3 blood_red = vec3(0.78, 0.04, 0.05);
    vec3 ember_red = vec3(1.0, 0.24, 0.08);
    vec3 hot_red = mix(deep_red, blood_red, ember_energy);
    hot_red = mix(hot_red, ember_red, smoothstep(0.45, 1.0, ember_energy));
    vec3 rim_red = vec3(1.0, 0.0, 0.0);
    vec3 alien_tint = mix(hot_red, rim_red, smoothstep(0.35, 1.0, red_energy + red_base * 0.4));

    float tooth_density = u_rat_tooth_count > 0.0 ? u_rat_tooth_count : 8.0;
    float max_tooth_height_px = max(1.0, floor(u_resolution.y * 0.10));
    float tooth_height_min_px =
        clamp(u_rat_tooth_height_min > 0.0 ? u_rat_tooth_height_min : 24.0, 1.0, max_tooth_height_px);
    float tooth_height_max_px =
        clamp(u_rat_tooth_height_max > 0.0 ? u_rat_tooth_height_max : 60.0, tooth_height_min_px, max_tooth_height_px);
    float tooth_base_min = u_rat_tooth_base_min > 0.0 ? u_rat_tooth_base_min : 0.012;
    float tooth_base_max = u_rat_tooth_base_max > tooth_base_min ? u_rat_tooth_base_max : 0.034;
    float tooth_animation_seconds = u_rat_tooth_animation_seconds > 0.01 ? u_rat_tooth_animation_seconds : 3.0;
    float tooth_fade_in_seconds = u_rat_tooth_fade_in_seconds > 0.0 ? u_rat_tooth_fade_in_seconds : 0.18;
    float tooth_fade_out_seconds = u_rat_tooth_fade_out_seconds > 0.0 ? u_rat_tooth_fade_out_seconds : 0.32;
    float tooth_fade_start_opacity = u_rat_tooth_fade_start_opacity;
    float tooth_fade_end_opacity = u_rat_tooth_fade_end_opacity;

    float density_phase = clamp((tooth_density - 1.0) / 23.0, 0.0, 1.0);
    vec2 edge_normal = normalize(edge_direction + vec2(0.0001));
    vec2 edge_tangent = vec2(-edge_normal.y, edge_normal.x);
    vec2 density_cell_size = mix(vec2(42.0, 30.0), vec2(16.0, 12.0), density_phase);
    vec2 density_cell = floor((uv * u_resolution) / density_cell_size);
    float density_seed = hash21(density_cell + vec2(13.7, 3.1));
    float edge_gate = smoothstep(0.06, 0.30, edge_strength + red_focus * 0.25);
    float local_activation = smoothstep(0.18, 0.92, edge_gate * (0.45 + 0.55 * density_seed));

    float tooth_variation = pow(density_seed, mix(0.65, 1.85, hash11(density_seed + 9.3)));
    float tooth_height_scale = mix(0.45, 1.0, hash11(density_seed + 15.9));
    float tooth_height_px = clamp(mix(tooth_height_min_px, tooth_height_max_px, tooth_variation) * tooth_height_scale,
                                  1.0, max_tooth_height_px);
    float tooth_jaggedness = mix(0.25, 0.85, hash11(density_seed + 37.3));
    float tooth_anim_speed = mix(0.55, 1.75, hash11(density_seed + 45.1));
    float tooth_anim_offset = hash11(density_seed + 52.7);
    float tooth_cycle = fract(u_time / tooth_animation_seconds * tooth_anim_speed + tooth_anim_offset +
                              sin(u_time * mix(0.14, 0.38, hash11(density_seed + 61.1)) + density_seed * 6.13) * 0.07);
    float tooth_visibility =
        tooth_cycle_envelope(tooth_cycle, tooth_fade_in_seconds, tooth_fade_out_seconds, tooth_animation_seconds,
                             tooth_fade_start_opacity, tooth_fade_end_opacity, density_seed + 52.7, tooth_jaggedness);
    tooth_visibility *= mix(0.42, 1.0, density_seed);

    float tooth_height_live = tooth_height_px * mix(0.26, 1.0, tooth_visibility);
    float tooth_base_px =
        max(mix(tooth_base_min, tooth_base_max, hash11(density_seed + 11.7)) * min(u_resolution.x, u_resolution.y),
            tooth_height_live * 0.14);
    tooth_base_px *= mix(0.28, 1.0, tooth_visibility);
    float tooth_skew = mix(0.34, 0.72, hash11(density_seed + 17.3));
    float tooth_lean = -mix(0.04, 0.18, hash11(density_seed + 21.7));
    float tooth_tip_shift = -tooth_base_px * mix(0.18, 0.46, hash11(density_seed + 29.1));
    float tooth_phase_jitter = (hash11(density_seed + 43.9) - 0.5) * mix(0.15, 0.42, tooth_jaggedness);
    float tooth_cell_jitter_px =
        (hash11(density_seed + 58.9) - 0.5) * tooth_height_live * mix(0.08, 0.28, density_phase);

    float edge_distance_px = (1.0 - edge_gate) * tooth_height_live + tooth_cell_jitter_px;
    float lateral_phase = fract(dot(density_cell, vec2(0.73, 0.27)) + tooth_phase_jitter + tooth_anim_offset);
    float lateral_px = (lateral_phase - 0.5) * tooth_base_px * 2.0;
    vec2 tooth_point = vec2(lateral_px, edge_distance_px);
    float tooth_body =
        saw_blade_tooth_shape(tooth_point, tooth_height_live, tooth_base_px, tooth_lean * tooth_height_live * 0.18,
                              tooth_skew, tooth_tip_shift, tooth_jaggedness, density_seed, tooth_cycle);
    float tooth_outer =
        saw_blade_tooth_shape(tooth_point + vec2(0.0, tooth_height_live * 0.12), tooth_height_live * 1.05,
                              tooth_base_px * 1.08, tooth_lean * tooth_height_live * 0.18, tooth_skew,
                              tooth_tip_shift * 1.05, tooth_jaggedness, hash11(density_seed + 33.7), tooth_cycle);
    float growth_shape = tooth_body;
    float tooth_outline = max(tooth_outer - tooth_body, 0.0);
    float edge_band = local_activation * (1.0 - smoothstep(0.0, tooth_height_live + 6.0, edge_distance_px));
    float edge_motion = 0.55 + 0.45 * sin(u_time * mix(0.6, 2.1, tooth_jaggedness) + density_seed * 1.71 +
                                          tooth_anim_offset * 6.28318530718);
    float edge_strength_final = clamp((edge_gate * local_activation + tooth_visibility * 0.15) * edge_motion, 0.0, 1.0);
    float alien_growth = growth_shape * edge_band * 2.60 * tooth_visibility * edge_gate;

    float tooth_chroma =
        edge_strength_final * (0.25 + 0.55 * tooth_jaggedness) * texel.x * (1.0 + 0.35 * tooth_visibility);
    vec2 tooth_distortion = edge_normal * edge_strength_final * texel * (4.5 + tooth_height_live * 0.10);
    tooth_distortion += edge_tangent * texel * (0.35 - density_seed) * tooth_base_px * 0.08 * edge_strength_final;
    vec2 tooth_uv = clamp(distorted_uv - tooth_distortion, 0.0, 1.0);
    vec3 tooth_sample = vec3(texture2D(u_texture, clamp(tooth_uv + vec2(tooth_chroma * 0.6, 0.0), 0.0, 1.0)).r,
                             texture2D(u_texture, tooth_uv).g,
                             texture2D(u_texture, clamp(tooth_uv - vec2(tooth_chroma * 0.6, 0.0), 0.0, 1.0)).b);
    color = mix(color, tooth_sample, clamp(edge_strength_final * 0.20 + alien_growth * 0.12, 0.0, 1.0));

    vec3 tooth_tint = mix(alien_tint, vec3(1.0, 0.0, 0.0), max(red_focus, edge_gate));
    vec3 tooth_body_tint = mix(vec3(0.62, 0.0, 0.0), vec3(1.0, 0.05, 0.03), max(red_focus, edge_gate));
    vec3 tooth_outline_tint = mix(vec3(0.18, 0.0, 0.0), vec3(0.45, 0.01, 0.01), max(red_focus, edge_gate));
    color += tooth_tint * alien_growth * (0.82 + red_energy * 0.55 + edge_gate * 1.20);
    color = mix(color, tooth_outline_tint,
                clamp(tooth_outline * edge_band * tooth_visibility * (0.72 + 0.18 * tooth_jaggedness), 0.0, 1.0));
    color = mix(color, tooth_body_tint,
                clamp(tooth_body * edge_band * tooth_visibility * (0.86 + 0.14 * tooth_jaggedness), 0.0, 1.0));
    color = mix(color, tooth_tint, clamp(alien_growth * 0.45, 0.0, 1.0));

    color = mix(base.rgb, color, clamp(edge_strength_final + alien_growth * 0.55, 0.0, 1.0));
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
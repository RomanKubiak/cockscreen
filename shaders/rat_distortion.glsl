precision mediump float;

varying vec2 v_texcoord;

uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;

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
    vec2 tangent = vec2(-edge_direction.y, edge_direction.x);

    vec2 distortion = edge_direction * edge_strength * (0.65 + 0.35 * sin(u_time * 2.2 + uv.y * 18.0)) * texel * 4.5;
    vec2 distorted_uv = clamp(uv - distortion, 0.0, 1.0);

    float chroma = edge_strength * (0.4 + 0.6 * sin(u_time * 2.7 + uv.y * 18.0)) * texel.x * 2.0;
    vec3 color = vec3(texture2D(u_texture, clamp(distorted_uv + vec2(chroma, 0.0), 0.0, 1.0)).r,
                      texture2D(u_texture, distorted_uv).g,
                      texture2D(u_texture, clamp(distorted_uv - vec2(chroma, 0.0), 0.0, 1.0)).b);

    vec3 edge_average = 0.25 * (sample_right + sample_left + sample_up + sample_down);
    vec3 contrast_color = abs(edge_average - base.rgb);
    float dominant = max(max(contrast_color.r, contrast_color.g), contrast_color.b);
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

    vec2 grid = uv * u_resolution / 128.0;
    vec2 cell_id = floor(grid);
    vec2 cell_uv = fract(grid) - 0.5;
    float phase = floor(u_time * 5.0);
    float pulse = hash21(cell_id + phase * 0.73);
    float pulse_gate = step(0.64, pulse);
    float pulse_age = fract(u_time * 5.0 + hash21(cell_id + 11.0));
    float growth_window = smoothstep(0.0, 0.12, pulse_age) * (1.0 - smoothstep(0.74, 0.99, pulse_age));

    vec2 local = vec2(dot(cell_uv, tangent), dot(cell_uv, edge_direction));
    float crawl_phase = u_time * (0.70 + hash21(cell_id + 20.0) * 0.9) + hash21(cell_id + 25.0) * 6.28318530718;
    float crawl_tangent = sin(crawl_phase) * 0.16;
    float crawl_depth = cos(crawl_phase * 0.63) * 0.08;
    vec2 crawling_local = local + vec2(crawl_tangent, crawl_depth);

    float primary_height = 1.10 + hash21(cell_id + 3.1) * 0.90;
    float primary_width = 0.028 + hash21(cell_id + 9.4) * 0.026;
    float angle_a = mix(-0.12, 0.16, hash21(cell_id + 2.3));
    float angle_b = mix(-0.52, -0.10, hash21(cell_id + 5.7));
    float angle_c = mix(0.10, 0.58, hash21(cell_id + 8.9));
    float angle_d = mix(-0.38, 0.38, hash21(cell_id + 13.7));

    vec2 thorn_a_point = rotate2(angle_a) * (crawling_local + vec2(0.0, 0.06));
    vec2 thorn_b_point = rotate2(angle_b) * (crawling_local + vec2(0.08, 0.04));
    vec2 thorn_c_point = rotate2(angle_c) * (crawling_local + vec2(-0.09, 0.05));
    vec2 thorn_d_point = rotate2(angle_d) * (crawling_local + vec2(0.03, 0.02));

    float thorn_a = thorn_shape(thorn_a_point, primary_height, primary_width, 0.16, 0.022);
    float thorn_b = thorn_shape(thorn_b_point, primary_height * (0.72 + hash21(cell_id + 1.6) * 0.22),
                                primary_width * (0.72 + hash21(cell_id + 4.6) * 0.18), -0.22, -0.018);
    float thorn_c = thorn_shape(thorn_c_point, primary_height * (0.66 + hash21(cell_id + 7.6) * 0.26),
                                primary_width * (0.64 + hash21(cell_id + 10.6) * 0.18), 0.24, 0.020);
    float thorn_d = thorn_shape(thorn_d_point, primary_height * (0.52 + hash21(cell_id + 12.6) * 0.18),
                                primary_width * (0.52 + hash21(cell_id + 15.6) * 0.12), -0.12, -0.014);
    float growth_shape = max(max(thorn_a, thorn_b), max(thorn_c, thorn_d));
    float edge_band = smoothstep(0.0, 0.04, edge_strength) * (1.0 - smoothstep(0.10, 0.42, abs(local.y)));
    float alien_growth = growth_shape * edge_band * pulse_gate * growth_window * red_focus * 1.65;

    float halo = smoothstep(0.34, 0.0, abs(crawling_local.x)) * smoothstep(1.10, 0.0, abs(crawling_local.y - 0.36));
    float root_glow = smoothstep(0.16, 0.0, length(vec2(crawling_local.x * 2.2, crawling_local.y - 0.02)));
    alien_growth = max(alien_growth, halo * edge_strength * pulse_gate * growth_window * 0.82);
    alien_growth = max(alien_growth, root_glow * edge_strength * pulse_gate * growth_window * 0.52);

    color += alien_tint * alien_growth * (1.30 + red_energy * 1.55 + edge_strength * 1.45);
    color = mix(color, alien_tint, clamp(alien_growth * 0.88, 0.0, 1.0));

    color = mix(base.rgb, color, clamp(edge_strength + alien_growth * 0.55, 0.0, 1.0));
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
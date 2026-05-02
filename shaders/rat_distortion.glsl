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

float triangle_shape(vec2 point, float height, float half_width)
{
    float y = point.y / max(height, 0.0001);
    float width_at_y = mix(half_width, 0.0, clamp(y, 0.0, 1.0));
    float inside = step(0.0, point.y) * step(point.y, height) * step(abs(point.x), width_at_y);
    return inside;
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
    float color_edge = length(color_dx) + length(color_dy);
    float edge_strength = smoothstep(0.04, 0.32, length(luma_gradient) + color_edge * 0.45);
    vec2 edge_direction =
        normalize(luma_gradient + vec2(color_dx.r - color_dx.b, color_dy.g - color_dy.r) * 0.2 + vec2(0.0001));
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
    vec3 detected_edge_color = dominant > 0.0001 ? contrast_color / dominant : vec3(1.0, 0.18, 0.08);
    float red_energy =
        clamp(contrast_color.r + dominant * 0.45 - (contrast_color.g + contrast_color.b) * 0.18, 0.0, 1.0);
    float ember_energy = clamp(red_energy * 0.7 + edge_strength * 0.45 + dominant * 0.35, 0.0, 1.0);
    vec3 deep_red = vec3(0.24, 0.01, 0.02);
    vec3 blood_red = vec3(0.78, 0.04, 0.05);
    vec3 ember_red = vec3(1.0, 0.24, 0.08);
    vec3 hot_red = mix(deep_red, blood_red, ember_energy);
    hot_red = mix(hot_red, ember_red, smoothstep(0.45, 1.0, ember_energy));
    vec3 red_edge_tint =
        mix(hot_red, vec3(detected_edge_color.r, detected_edge_color.g * 0.18, detected_edge_color.b * 0.12), 0.22);
    vec3 alien_tint = mix(red_edge_tint, ember_red, smoothstep(0.6, 1.0, dominant + red_energy * 0.35));

    vec2 grid = uv * u_resolution / 42.0;
    vec2 cell_id = floor(grid);
    vec2 cell_uv = fract(grid) - 0.5;
    float phase = floor(u_time * 4.0);
    float pulse = hash21(cell_id + phase * 0.73);
    float pulse_gate = step(0.78, pulse);
    float pulse_age = fract(u_time * 4.0 + hash21(cell_id + 11.0));
    float growth_window = smoothstep(0.0, 0.18, pulse_age) * (1.0 - smoothstep(0.58, 0.96, pulse_age));

    vec2 local = vec2(dot(cell_uv, tangent), dot(cell_uv, edge_direction));
    float spike_height = 0.16 + hash21(cell_id + 3.1) * 0.26;
    float spike_width = 0.05 + hash21(cell_id + 9.4) * 0.10;
    float spike_a = triangle_shape(local + vec2(0.0, 0.18), spike_height, spike_width);
    float spike_b = triangle_shape(vec2(local.x * 1.4 + 0.08, local.y + 0.04), spike_height * 0.78, spike_width * 0.55);
    float spike_c = triangle_shape(vec2(local.x * 1.2 - 0.09, local.y + 0.08), spike_height * 0.62, spike_width * 0.45);
    float growth_shape = max(spike_a, max(spike_b, spike_c));
    float edge_band = smoothstep(0.0, 0.12, edge_strength) * (1.0 - smoothstep(0.12, 0.42, abs(local.y)));
    float alien_growth = growth_shape * edge_band * pulse_gate * growth_window;

    float halo = smoothstep(0.38, 0.0, abs(local.x)) * smoothstep(0.42, 0.0, abs(local.y - 0.12));
    alien_growth = max(alien_growth, halo * edge_strength * pulse_gate * growth_window * 0.35);

    color += alien_tint * alien_growth * (0.45 + 0.55 * dominant + edge_strength * 0.6);
    color = mix(color, alien_tint, alien_growth * 0.35);

    color = mix(base.rgb, color, 0.35 + edge_strength * 0.65);
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
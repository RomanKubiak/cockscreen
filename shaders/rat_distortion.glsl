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

    vec2 grid = uv * u_resolution / 168.0;
    vec2 cell_uv = fract(grid) - 0.5;
    vec2 local = vec2(dot(cell_uv, tangent), dot(cell_uv, edge_direction));
    vec2 tooth_local = vec2(local.x * 1.35, abs(local.y));

    float tooth_height = 1.75;
    float tooth_base = 0.022;
    float primary_tooth = triangle_shape(tooth_local + vec2(0.0, 0.01), tooth_height, tooth_base);
    float side_tooth_left =
        triangle_shape(vec2((local.x + 0.34) * 1.1, abs(local.y) + 0.03), tooth_height * 0.72, tooth_base * 0.78);
    float side_tooth_right =
        triangle_shape(vec2((local.x - 0.34) * 1.1, abs(local.y) + 0.03), tooth_height * 0.72, tooth_base * 0.78);
    float growth_shape = max(primary_tooth, max(side_tooth_left, side_tooth_right));
    float edge_band = smoothstep(0.0, 0.035, edge_strength) * (1.0 - smoothstep(0.05, 0.20, abs(local.y)));
    float alien_growth = growth_shape * edge_band * red_focus * 2.10;

    float halo = smoothstep(0.20, 0.0, abs(local.x)) * smoothstep(1.35, 0.0, abs(abs(local.y) - 0.44));
    float root_glow = smoothstep(0.10, 0.0, length(vec2(local.x * 3.0, abs(local.y) - 0.02)));
    alien_growth = max(alien_growth, halo * edge_strength * 0.95);
    alien_growth = max(alien_growth, root_glow * edge_strength * 0.65);

    vec2 tooth_distortion = edge_direction * alien_growth * texel * 16.0;
    vec2 tooth_uv = clamp(distorted_uv - tooth_distortion, 0.0, 1.0);
    vec3 tooth_sample = vec3(texture2D(u_texture, clamp(tooth_uv + vec2(chroma * 0.6, 0.0), 0.0, 1.0)).r,
                             texture2D(u_texture, tooth_uv).g,
                             texture2D(u_texture, clamp(tooth_uv - vec2(chroma * 0.6, 0.0), 0.0, 1.0)).b);
    color = mix(color, tooth_sample, clamp(alien_growth * 0.55, 0.0, 1.0));

    color += alien_tint * alien_growth * (1.30 + red_energy * 1.55 + edge_strength * 1.45);
    color = mix(color, alien_tint, clamp(alien_growth * 0.88, 0.0, 1.0));

    color = mix(base.rgb, color, clamp(edge_strength + alien_growth * 0.55, 0.0, 1.0));
    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
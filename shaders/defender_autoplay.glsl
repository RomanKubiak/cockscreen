// Auto-playing Defender-inspired flyby with a crisp raster/vector look.

const float kTau = 6.28318530718;

const int GLYPH_H = 10;
const int GLYPH_I = 11;
const int GLYPH_U = 12;
const int GLYPH_P = 13;
const int GLYPH_Z = 14;
const int GLYPH_O = 15;
const int GLYPH_N = 16;
const int GLYPH_E = 17;
const int GLYPH_T = 18;
const int GLYPH_B = 19;

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

mat2 rotate2(float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return mat2(c, -s, s, c);
}

vec3 palette(float t)
{
    return 0.45 + 0.55 * cos(kTau * (vec3(0.08, 0.26, 0.61) + t));
}

float viewport_pixel()
{
    return 1.0 / max(iResolution.y, 1.0);
}

float line_mask(vec2 p, vec2 a, vec2 b, float width_pixels);

vec3 rotate_x_axis(vec3 p, float angle)
{
    float c = cos(angle);
    float s = sin(angle);
    return vec3(p.x, c * p.y - s * p.z, s * p.y + c * p.z);
}

vec2 project_forward_vertex(vec3 vertex, float angle, float scale_pixels, float depth_scale)
{
    vec3 rotated = rotate_x_axis(vec3(vertex.x, vertex.y, vertex.z * depth_scale), angle);
    float perspective = 1.0 / max(3.4 - rotated.z, 0.8);
    return rotated.xy * perspective * scale_pixels * viewport_pixel();
}

float projected_segment_mask(vec2 p, vec3 a, vec3 b, float angle, float scale_pixels, float depth_scale,
                             float width_pixels)
{
    vec2 pa = project_forward_vertex(a, angle, scale_pixels, depth_scale);
    vec2 pb = project_forward_vertex(b, angle, scale_pixels, depth_scale);
    return line_mask(p, pa, pb, width_pixels);
}

float segment_distance(vec2 p, vec2 a, vec2 b)
{
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.0001), 0.0, 1.0);
    return length(pa - ba * h);
}

float line_mask(vec2 p, vec2 a, vec2 b, float width_pixels)
{
    return step(segment_distance(p, a, b), viewport_pixel() * width_pixels);
}

float box_mask(vec2 p, vec2 half_size)
{
    vec2 d = abs(p) - half_size;
    return step(max(d.x, d.y), 0.0);
}

float ellipse_mask(vec2 p, vec2 radii)
{
    vec2 q = p / radii;
    return step(dot(q, q), 1.0);
}

float ring_mask(vec2 p, float radius, float width)
{
    return step(abs(length(p) - radius), width);
}

float terrain_wave(float x)
{
    return sin(x * 0.85) + 0.45 * sin(x * 1.8 + 1.4) + 0.20 * sin(x * 4.3 - 0.8);
}

float ground_height(float x)
{
    float ridges = terrain_wave(x) * 0.10;
    float cliffs = pow(max(sin(x * 0.22 + 0.6), 0.0), 5.0) * 0.22;
    return -0.33 + ridges + cliffs;
}

float screen_to_world_x(float screen_x, float time)
{
    return screen_x * 3.2 + time * 1.15;
}

float world_to_screen_x(float world_x, float time)
{
    return (world_x - time * 1.15) / 3.2;
}

float ground_screen_y(float screen_x, float time)
{
    return ground_height(screen_to_world_x(screen_x, time));
}

vec2 enemy_position(float fi, float seed, float time)
{
    float cycle = fract(time * (0.18 + fi * 0.012) + seed * 0.173);
    float enemy_x = mix(1.12 + mod(fi, 3.0) * 0.10, -1.14 - mod(fi, 2.0) * 0.08, cycle);
    float enemy_y = 0.08 + 0.20 * sin(time * (0.90 + fi * 0.10) + seed * 1.7) +
                    0.06 * sin(time * (2.0 + mod(fi, 4.0) * 0.13) + seed * 2.7);
    return vec2(enemy_x, enemy_y);
}

float enemy_points(float fi)
{
    return 175.0 + fi * 30.0;
}

float horizontal_segment_hits_box(vec2 start_point, vec2 end_point, vec2 center, vec2 half_size)
{
    float min_x = min(start_point.x, end_point.x);
    float max_x = max(start_point.x, end_point.x);
    float overlap_x = step(center.x - half_size.x, max_x) * step(min_x, center.x + half_size.x);
    float overlap_y = step(abs(start_point.y - center.y), half_size.y);
    return overlap_x * overlap_y;
}

float ridge_far(float x)
{
    return 0.16 + terrain_wave(x * 0.30 - 6.0) * 0.11;
}

float ridge_mid(float x)
{
    return -0.03 + terrain_wave(x * 0.46 + 3.5) * 0.08;
}

float star_layer(vec2 uv, float scale, float speed, float density, float seed)
{
    vec2 grid = vec2((uv.x + iTime * speed + seed) * scale, (uv.y + seed * 0.19) * scale);
    vec2 id = floor(grid);
    vec2 local = fract(grid) - 0.5;
    vec2 jitter = (vec2(hash12(id + seed * 5.1), hash12(id + seed * 9.7)) - 0.5) * 0.70;
    float star = step(density, hash12(id + seed * 11.0));
    float twinkle = step(0.45, hash12(id + floor(iTime * (2.0 + seed * 0.3)) + 29.0));
    float point = step(length(local - jitter), 0.11);
    return star * twinkle * point;
}

vec2 ship_position(float time)
{
    float ship_x = -0.50;
    float world_x = screen_to_world_x(ship_x, time);
    float terrain = ground_height(world_x);
    float ship_y = terrain + 0.22 + 0.04 * sin(time * 1.6) + 0.02 * sin(time * 3.4 + 0.9);
    return vec2(ship_x, ship_y);
}

float ship_angle(float time)
{
    return time * 0.95;
}

vec3 render_ship(vec2 p, vec2 ship_pos, float time)
{
    float angle = ship_angle(time);
    vec2 q = p - ship_pos;
    float scale = 24.0;
    float depth = 1.0;

    vec3 nose = vec3(1.7, 0.0, 0.0);
    vec3 base_a = vec3(-1.1, -0.9, -0.9);
    vec3 base_b = vec3(-1.1, 0.9, -0.9);
    vec3 base_c = vec3(-1.1, 0.9, 0.9);
    vec3 base_d = vec3(-1.1, -0.9, 0.9);

    float hull = 0.0;
    hull += projected_segment_mask(q, nose, base_a, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, nose, base_b, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, nose, base_c, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, nose, base_d, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, base_a, base_b, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, base_b, base_c, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, base_c, base_d, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, base_d, base_a, angle, scale, depth, 0.75);

    return vec3(0.18, 0.95, 1.0) * step(0.5, hull);
}

float explosion_mask(vec2 p, float phase)
{
    if (phase <= 0.0)
    {
        return 0.0;
    }

    float px = viewport_pixel();
    float radius = (2.0 + phase * 8.0) * px;
    float ring = ring_mask(p, radius, px * 0.75);
    float spokes = step(0.88, abs(sin(atan(p.y, p.x) * 6.0 + phase * 14.0)));
    float core = step(length(p), (1.0 + (1.0 - phase) * 2.0) * px);
    return clamp(ring * spokes + core * (1.0 - step(0.7, phase)), 0.0, 1.0);
}

float enemy_variant(float seed)
{
    return mod(floor(seed), 5.0);
}

float enemy_scale(float variant)
{
    return 20.0 + variant * 2.6;
}

float enemy_spin_speed(float seed)
{
    return 0.70 + hash11(seed * 5.3 + 7.1) * 1.45;
}

float enemy_spin_intensity(float seed)
{
    return 0.65 + hash11(seed * 3.7 + 19.0) * 0.85;
}

vec2 enemy_hit_half_extent(float variant)
{
    float scale = enemy_scale(variant);
    return vec2(0.72, 0.56) * scale * viewport_pixel();
}

float fragment_wire(vec2 q, float angle, float scale, float depth)
{
    vec3 nose = vec3(1.2, 0.0, 0.0);
    vec3 base_a = vec3(-0.9, -0.7, -0.6);
    vec3 base_b = vec3(-0.9, 0.7, -0.6);
    vec3 base_c = vec3(-0.9, 0.0, 0.8);

    float hull = 0.0;
    hull += projected_segment_mask(q, nose, base_a, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, nose, base_b, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, nose, base_c, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, base_a, base_b, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, base_b, base_c, angle, scale, depth, 0.75);
    hull += projected_segment_mask(q, base_c, base_a, angle, scale, depth, 0.75);
    return step(0.5, hull);
}

vec3 render_debris_cluster(vec2 p, float impact_world_x, float impact_y, float age, float seed, vec3 tint, float time)
{
    if (age <= 0.0)
    {
        return vec3(0.0);
    }

    vec3 color = vec3(0.0);
    float fade = clamp(1.0 - age * 0.75, 0.0, 1.0);

    for (int i = 0; i < 6; ++i)
    {
        float fi = float(i);
        float frag_seed = seed * 13.7 + fi * 7.3;
        float drift_x = (hash11(frag_seed + 1.0) - 0.5) * age * 1.35;
        float drift_y = (hash11(frag_seed + 2.0) - 0.25) * age * 0.75 - age * age * 0.28;
        float world_x = impact_world_x + drift_x;
        vec2 center = vec2(world_to_screen_x(world_x, time), impact_y + drift_y);
        float spin = age * (5.0 + hash11(frag_seed + 3.0) * 8.0) + hash11(frag_seed + 4.0) * kTau;
        float scale = 4.0 + hash11(frag_seed + 5.0) * 3.0;
        float depth = 0.55 + hash11(frag_seed + 6.0) * 0.95;
        float fragment = fragment_wire(p - center, spin, scale, depth);
        color += tint * fragment * fade;
    }

    return color;
}

vec3 render_enemy(vec2 p, vec2 enemy_pos, float seed, float time, float explode)
{
    float px = viewport_pixel();
    vec2 q = p - enemy_pos;
    float variant = enemy_variant(seed);
    float scale = enemy_scale(variant);
    float angle = time * enemy_spin_speed(seed) + seed * 1.7;
    float depth = enemy_spin_intensity(seed);

    float frame = 0.0;
    if (variant < 0.5)
    {
        vec3 nose = vec3(1.5, 0.0, 0.0);
        vec3 base_a = vec3(-1.0, -0.8, -0.8);
        vec3 base_b = vec3(-1.0, 0.8, -0.8);
        vec3 base_c = vec3(-1.0, 0.8, 0.8);
        vec3 base_d = vec3(-1.0, -0.8, 0.8);
        frame += projected_segment_mask(q, nose, base_a, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, base_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, base_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, base_d, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, base_a, base_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, base_b, base_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, base_c, base_d, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, base_d, base_a, angle, scale, depth, 0.75);
    }
    else if (variant < 1.5)
    {
        vec3 nose = vec3(1.8, 0.0, 0.0);
        vec3 tail = vec3(-1.6, 0.0, 0.0);
        vec3 ring_a = vec3(0.0, -1.0, -1.0);
        vec3 ring_b = vec3(0.0, 1.0, -1.0);
        vec3 ring_c = vec3(0.0, 1.0, 1.0);
        vec3 ring_d = vec3(0.0, -1.0, 1.0);
        frame += projected_segment_mask(q, nose, ring_a, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, ring_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, ring_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, ring_d, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, ring_a, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, ring_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, ring_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, ring_d, angle, scale, depth, 0.75);
    }
    else if (variant < 2.5)
    {
        vec3 nose = vec3(1.6, 0.0, 0.0);
        vec3 mid_a = vec3(-0.2, -1.1, -0.9);
        vec3 mid_b = vec3(-0.2, 1.1, -0.9);
        vec3 mid_c = vec3(-0.2, 1.1, 0.9);
        vec3 mid_d = vec3(-0.2, -1.1, 0.9);
        vec3 tail = vec3(-1.5, 0.0, 0.0);
        frame += projected_segment_mask(q, nose, mid_a, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, mid_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, mid_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, mid_d, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, mid_a, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, mid_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, mid_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, mid_d, angle, scale, depth, 0.75);
    }
    else if (variant < 3.5)
    {
        vec3 nose = vec3(1.5, 0.0, 0.0);
        vec3 top = vec3(-0.6, 0.0, 1.2);
        vec3 bottom = vec3(-0.6, 0.0, -1.2);
        vec3 left = vec3(-0.6, -1.2, 0.0);
        vec3 right = vec3(-0.6, 1.2, 0.0);
        vec3 tail = vec3(-1.7, 0.0, 0.0);
        frame += projected_segment_mask(q, nose, top, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, bottom, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, left, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, right, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, top, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, bottom, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, left, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, right, angle, scale, depth, 0.75);
    }
    else
    {
        vec3 nose = vec3(1.9, 0.0, 0.0);
        vec3 tail = vec3(-1.4, 0.0, 0.0);
        vec3 wing_a = vec3(0.0, -1.3, -0.7);
        vec3 wing_b = vec3(0.0, 1.3, -0.7);
        vec3 wing_c = vec3(0.0, 1.3, 0.7);
        vec3 wing_d = vec3(0.0, -1.3, 0.7);
        frame += projected_segment_mask(q, nose, wing_a, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, wing_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, wing_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, nose, wing_d, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, wing_a, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, wing_b, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, wing_c, angle, scale, depth, 0.75);
        frame += projected_segment_mask(q, tail, wing_d, angle, scale, depth, 0.75);
    }
    frame = step(0.5, frame);

    vec2 core = project_forward_vertex(vec3(0.3, 0.0, 0.0), angle, scale, depth);
    float eye = box_mask(q - core, vec2(1.3, 1.3) * px);
    vec3 tint = palette(seed * 0.19 + time * 0.015);
    tint = mix(tint, tint.gbr, 0.25);
    float destroyed = step(0.001, explode);

    vec3 color = tint * frame * (1.0 - destroyed);
    color += vec3(1.0, 0.90, 0.30) * eye * step(0.45, sin(time * 8.0 + seed * 11.0) * 0.5 + 0.5) * (1.0 - destroyed);
    return color;
}

vec3 render_population(vec2 p, float time)
{
    vec3 color = vec3(0.0);
    float px = viewport_pixel();
    float cell_origin = floor((time * 1.15) / 0.42);

    for (int i = 0; i < 12; ++i)
    {
        float id = cell_origin + float(i) - 5.0;
        float world_x = id * 0.42 + hash11(id + 3.0) * 0.16;
        float screen_x = (world_x - time * 1.15) / 3.2;
        float terrain = ground_height(world_x);
        float tower_height = 0.035 + floor(hash11(id + 9.0) * 4.0) * 0.014;
        float tower = line_mask(p, vec2(screen_x, terrain + 0.008), vec2(screen_x, terrain + tower_height), 0.75);
        float cap = box_mask(p - vec2(screen_x, terrain + tower_height + 2.0 * px), vec2(2.0, 1.5) * px);
        float human_offset = mix(-5.0, 5.0, step(0.5, fract(id * 0.37))) * px;
        float human = box_mask(p - vec2(screen_x + human_offset, terrain + 3.0 * px), vec2(1.0, 2.0) * px);
        vec3 tint = palette(id * 0.07 + 0.15);

        color += tint * tower * 0.30;
        color += vec3(0.95, 0.75, 0.20) * cap * 0.22;
        color += vec3(0.95, 0.90, 0.75) * human * 0.20;
    }

    return color;
}

float rect_fill(vec2 cell, vec2 min_cell, vec2 max_cell)
{
    return step(min_cell.x, cell.x) * step(min_cell.y, cell.y) * step(cell.x, max_cell.x) * step(cell.y, max_cell.y);
}

float rect_outline(vec2 cell, vec2 min_cell, vec2 max_cell)
{
    float outer = rect_fill(cell, min_cell, max_cell);
    float inner = rect_fill(cell, min_cell + vec2(1.0), max_cell - vec2(1.0));
    return max(outer - inner, 0.0);
}

float cell_eq(float value, float target)
{
    return 1.0 - step(0.5, abs(value - target));
}

float row3(vec2 cell, float row, float pattern)
{
    if (abs(cell.y - row) >= 0.5)
    {
        return 0.0;
    }

    float bits = pattern;
    float mask = 0.0;
    if (bits >= 4.0)
    {
        mask += cell_eq(cell.x, 0.0);
        bits -= 4.0;
    }
    if (bits >= 2.0)
    {
        mask += cell_eq(cell.x, 1.0);
        bits -= 2.0;
    }
    if (bits >= 1.0)
    {
        mask += cell_eq(cell.x, 2.0);
    }

    return step(0.5, mask);
}

float glyph_rows(vec2 cell, float r0, float r1, float r2, float r3, float r4)
{
    return row3(cell, 0.0, r0) + row3(cell, 1.0, r1) + row3(cell, 2.0, r2) + row3(cell, 3.0, r3) + row3(cell, 4.0, r4);
}

float glyph3(vec2 local, int glyph)
{
    vec2 cell = floor(local);
    if (cell.x < 0.0 || cell.x > 2.0 || cell.y < 0.0 || cell.y > 4.0)
    {
        return 0.0;
    }

    float mask = 0.0;
    if (glyph == 0)
    {
        mask = glyph_rows(cell, 7.0, 5.0, 5.0, 5.0, 7.0);
    }
    else if (glyph == 1)
    {
        mask = glyph_rows(cell, 2.0, 6.0, 2.0, 2.0, 7.0);
    }
    else if (glyph == 2)
    {
        mask = glyph_rows(cell, 7.0, 1.0, 7.0, 4.0, 7.0);
    }
    else if (glyph == 3)
    {
        mask = glyph_rows(cell, 7.0, 1.0, 7.0, 1.0, 7.0);
    }
    else if (glyph == 4)
    {
        mask = glyph_rows(cell, 5.0, 5.0, 7.0, 1.0, 1.0);
    }
    else if (glyph == 5)
    {
        mask = glyph_rows(cell, 7.0, 4.0, 7.0, 1.0, 7.0);
    }
    else if (glyph == 6)
    {
        mask = glyph_rows(cell, 7.0, 4.0, 7.0, 5.0, 7.0);
    }
    else if (glyph == 7)
    {
        mask = glyph_rows(cell, 7.0, 1.0, 1.0, 1.0, 1.0);
    }
    else if (glyph == 8)
    {
        mask = glyph_rows(cell, 7.0, 5.0, 7.0, 5.0, 7.0);
    }
    else if (glyph == 9)
    {
        mask = glyph_rows(cell, 7.0, 5.0, 7.0, 1.0, 7.0);
    }
    else if (glyph == GLYPH_H)
    {
        mask = glyph_rows(cell, 5.0, 5.0, 7.0, 5.0, 5.0);
    }
    else if (glyph == GLYPH_I)
    {
        mask = glyph_rows(cell, 7.0, 2.0, 2.0, 2.0, 7.0);
    }
    else if (glyph == GLYPH_U)
    {
        mask = glyph_rows(cell, 5.0, 5.0, 5.0, 5.0, 7.0);
    }
    else if (glyph == GLYPH_P)
    {
        mask = glyph_rows(cell, 6.0, 5.0, 6.0, 4.0, 4.0);
    }
    else if (glyph == GLYPH_Z)
    {
        mask = glyph_rows(cell, 7.0, 1.0, 2.0, 4.0, 7.0);
    }
    else if (glyph == GLYPH_O)
    {
        mask = glyph_rows(cell, 7.0, 5.0, 5.0, 5.0, 7.0);
    }
    else if (glyph == GLYPH_N)
    {
        mask = glyph_rows(cell, 5.0, 7.0, 7.0, 7.0, 5.0);
    }
    else if (glyph == GLYPH_E)
    {
        mask = glyph_rows(cell, 7.0, 4.0, 6.0, 4.0, 7.0);
    }
    else if (glyph == GLYPH_T)
    {
        mask = glyph_rows(cell, 7.0, 2.0, 2.0, 2.0, 2.0);
    }
    else if (glyph == GLYPH_B)
    {
        mask = glyph_rows(cell, 6.0, 5.0, 6.0, 5.0, 6.0);
    }

    return step(0.5, mask);
}

float draw_char(vec2 cell, vec2 origin, int glyph)
{
    return glyph3(cell - origin, glyph);
}

float draw_number(vec2 cell, vec2 origin, float value, int digits)
{
    float safe_value = floor(max(value, 0.0));
    float mask = 0.0;

    for (int i = 0; i < 8; ++i)
    {
        if (i < digits)
        {
            int place = digits - 1 - i;
            float divisor = pow(10.0, float(place));
            int digit = int(mod(floor(safe_value / divisor), 10.0));
            mask += draw_char(cell, origin + vec2(float(i) * 4.0, 0.0), digit);
        }
    }

    return step(0.5, mask);
}

float seven_seg_digit(vec2 local, int digit)
{
    vec2 cell = floor(local);
    if (cell.x < 0.0 || cell.x > 5.0 || cell.y < 0.0 || cell.y > 8.0)
    {
        return 0.0;
    }

    float seg_a = rect_fill(cell, vec2(1.0, 0.0), vec2(4.0, 0.0));
    float seg_b = rect_fill(cell, vec2(5.0, 1.0), vec2(5.0, 3.0));
    float seg_c = rect_fill(cell, vec2(5.0, 5.0), vec2(5.0, 7.0));
    float seg_d = rect_fill(cell, vec2(1.0, 8.0), vec2(4.0, 8.0));
    float seg_e = rect_fill(cell, vec2(0.0, 5.0), vec2(0.0, 7.0));
    float seg_f = rect_fill(cell, vec2(0.0, 1.0), vec2(0.0, 3.0));
    float seg_g = rect_fill(cell, vec2(1.0, 4.0), vec2(4.0, 4.0));

    float mask = 0.0;
    if (digit == 0 || digit == 2 || digit == 3 || digit == 5 || digit == 6 || digit == 7 || digit == 8 || digit == 9)
    {
        mask += seg_a;
    }
    if (digit == 0 || digit == 1 || digit == 2 || digit == 3 || digit == 4 || digit == 7 || digit == 8 || digit == 9)
    {
        mask += seg_b;
    }
    if (digit == 0 || digit == 1 || digit == 3 || digit == 4 || digit == 5 || digit == 6 || digit == 7 || digit == 8 ||
        digit == 9)
    {
        mask += seg_c;
    }
    if (digit == 0 || digit == 2 || digit == 3 || digit == 5 || digit == 6 || digit == 8 || digit == 9)
    {
        mask += seg_d;
    }
    if (digit == 0 || digit == 2 || digit == 6 || digit == 8)
    {
        mask += seg_e;
    }
    if (digit == 0 || digit == 4 || digit == 5 || digit == 6 || digit == 8 || digit == 9)
    {
        mask += seg_f;
    }
    if (digit == 2 || digit == 3 || digit == 4 || digit == 5 || digit == 6 || digit == 8 || digit == 9)
    {
        mask += seg_g;
    }

    return step(0.5, mask);
}

float draw_seven_seg_number(vec2 cell, vec2 origin, float value, int digits)
{
    float safe_value = floor(max(value, 0.0));
    float mask = 0.0;

    for (int i = 0; i < 8; ++i)
    {
        if (i < digits)
        {
            int place = digits - 1 - i;
            float divisor = pow(10.0, float(place));
            int digit = int(mod(floor(safe_value / divisor), 10.0));
            vec2 local = cell - (origin + vec2(float(i) * 8.0, 0.0));
            mask += seven_seg_digit(local, digit);
        }
    }

    return step(0.5, mask);
}

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float px = viewport_pixel();
    vec2 uv = fragCoord / iResolution.xy;
    vec2 p = (fragCoord - 0.5 * iResolution.xy) / iResolution.y;
    float time = iTime;

    vec3 color = vec3(0.0);

    float world_x = screen_to_world_x(p.x, time);
    float ground = ground_height(world_x);
    float sky_floor = ground + px;
    float sky_mask = step(sky_floor, p.y);

    float stars_a = star_layer(uv, 220.0, 0.020, 0.9970, 3.0) * sky_mask;
    float stars_b = star_layer(uv + vec2(0.0, 0.07), 300.0, 0.035, 0.9980, 9.0) * sky_mask;
    float stars_c = star_layer(uv + vec2(0.0, 0.12), 380.0, 0.050, 0.9988, 15.0) * sky_mask;
    color += vec3(0.55, 0.78, 1.0) * stars_a;
    color += vec3(1.0, 0.52, 0.76) * stars_b;
    color += vec3(1.0, 0.88, 0.32) * stars_c;

    vec3 ground_line_color = mix(palette(world_x * 0.03 + 0.22), vec3(0.24, 1.0, 0.55), 0.50);
    color += ground_line_color * step(abs(p.y - ground), px * 0.75);
    color += render_population(p, time);

    vec2 ship_pos = ship_position(time);
    color += render_ship(p, ship_pos, time);

    float collision_score = 0.0;
    float collision_pulse_score = 0.0;
    for (int i = 0; i < 12; ++i)
    {
        float fi = float(i);
        float seed = fi + 1.0;
        vec2 enemy_pos = enemy_position(fi, seed, time);
        float variant = enemy_variant(seed);
        vec2 enemy_hit_half = enemy_hit_half_extent(variant);
        float enemy_ground = ground_screen_y(enemy_pos.x, time);

        float shot_rate = 0.25 + fi * 0.018;
        float shot_phase = fract(time * shot_rate + seed * 0.21);
        float projectile_live = step(0.48, shot_phase) * (1.0 - step(0.62, shot_phase));
        float projectile_t = clamp((shot_phase - 0.48) / 0.14, 0.0, 1.0);

        float shot_start_time = time;
        vec2 muzzle = vec2(0.0);
        vec2 projectile_center = vec2(0.0);
        vec2 projectile_start = vec2(0.0);
        vec2 projectile_end = vec2(0.0);
        float enemy_hit_phase = 2.0;
        float ground_hit_phase = 2.0;
        float enemy_impact_world_x = screen_to_world_x(enemy_pos.x, time);
        float enemy_impact_y = enemy_pos.y;
        float projectile_ground_world_x = screen_to_world_x(projectile_center.x, time);
        float projectile_ground_y = 0.0;

        if (shot_phase >= 0.48)
        {
            shot_start_time = time - (shot_phase - 0.48) / max(shot_rate, 0.001);
            vec2 shot_ship_pos = ship_position(shot_start_time);
            muzzle =
                shot_ship_pos + project_forward_vertex(vec3(1.7, 0.0, 0.0), ship_angle(shot_start_time), 24.0, 1.0);
            projectile_center = muzzle + vec2(projectile_t * 2.20, 0.0);
            projectile_start = projectile_center - vec2(5.0 * px, 0.0);
            projectile_end = projectile_center + vec2(4.0 * px, 0.0);

            vec2 expanded_enemy_hit = enemy_hit_half + vec2(7.0 * px, 9.0 * px);
            float shot_window_end = min(shot_phase, 0.62);
            for (int sample = 0; sample < 12; ++sample)
            {
                float sample_t = float(sample) / 11.0;
                float sample_phase = mix(0.48, 0.62, sample_t);
                if (sample_phase > shot_window_end)
                {
                    continue;
                }

                float sample_time = time - (shot_phase - sample_phase) / max(shot_rate, 0.001);
                vec2 sample_enemy_pos = enemy_position(fi, seed, sample_time);
                float sample_projectile_t = (sample_phase - 0.48) / 0.14;
                vec2 sample_center = muzzle + vec2(sample_projectile_t * 2.20, 0.0);
                vec2 sample_start = sample_center - vec2(5.0 * px, 0.0);
                vec2 sample_end = sample_center + vec2(4.0 * px, 0.0);
                float sample_ground_y =
                    max(ground_screen_y(sample_start.x, sample_time),
                        max(ground_screen_y(sample_center.x, sample_time), ground_screen_y(sample_end.x, sample_time)));

                if (enemy_hit_phase > 1.5 &&
                    horizontal_segment_hits_box(sample_start, sample_end, sample_enemy_pos, expanded_enemy_hit) > 0.5)
                {
                    enemy_hit_phase = sample_phase;
                    enemy_impact_world_x = screen_to_world_x(sample_enemy_pos.x, sample_time);
                    enemy_impact_y = sample_enemy_pos.y;
                }
                if (ground_hit_phase > 1.5 && sample_center.y <= sample_ground_y + px)
                {
                    ground_hit_phase = sample_phase;
                    projectile_ground_world_x = screen_to_world_x(sample_center.x, sample_time);
                    projectile_ground_y = sample_ground_y;
                }
            }
        }

        float enemy_explosion = 0.0;
        float enemy_hit_started = 0.0;
        if (enemy_hit_phase < 1.5 && shot_phase >= enemy_hit_phase)
        {
            enemy_hit_started = 1.0 - step(enemy_hit_phase + 0.03, shot_phase);
            enemy_explosion = clamp((shot_phase - enemy_hit_phase) / 0.24 + 0.04, 0.0, 1.0);
        }

        float ground_explosion = 0.0;
        if (ground_hit_phase < 1.5 && shot_phase >= ground_hit_phase)
        {
            ground_explosion = clamp((shot_phase - ground_hit_phase) / 0.18 + 0.05, 0.0, 1.0);
        }

        float enemy_ground_hit = step(enemy_pos.y - enemy_hit_half.y, enemy_ground + px);
        float enemy_ground_explosion = enemy_ground_hit * 0.70;
        float enemy_destroyed = max(enemy_explosion, enemy_ground_explosion);
        float projectile_blocked = max(step(enemy_hit_phase, shot_phase) * (1.0 - step(1.5, enemy_hit_phase)),
                                       step(ground_hit_phase, shot_phase) * (1.0 - step(1.5, ground_hit_phase)));
        float projectile_visible = projectile_live * (1.0 - projectile_blocked);

        collision_score += enemy_points(fi) * step(0.001, enemy_explosion);
        collision_pulse_score += enemy_points(fi) * enemy_hit_started;

        vec3 beam_color = mix(vec3(0.16, 0.92, 1.0), vec3(1.0, 0.90, 0.22), hash11(seed + 8.0));
        color += render_enemy(p, enemy_pos, seed, time, enemy_destroyed);
        color += beam_color * line_mask(p, projectile_start, projectile_end, 0.75) * projectile_visible;
        color += render_debris_cluster(p, enemy_impact_world_x, enemy_impact_y, enemy_explosion, seed,
                                       vec3(1.0, 0.58, 0.18), time);
        color += render_debris_cluster(p, projectile_ground_world_x, projectile_ground_y, ground_explosion, seed + 19.0,
                                       vec3(1.0, 0.85, 0.25), time);
    }

    float total_score = floor((3200.0 + floor(time * 110.0) * 10.0 + collision_score * 2.0) / 5.0) * 5.0;
    float hit_score = floor((collision_pulse_score + hash12(vec2(floor(time * 10.0), 7.0)) * 125.0) / 5.0) * 5.0;
    float bonus_score =
        floor((600.0 + floor(time * 14.0) * 15.0 + hash12(vec2(floor(time * 6.0), 19.0)) * 900.0) / 5.0) * 5.0;

    float hud_scale = max(floor(iResolution.y / 300.0), 2.0);
    vec2 cell = floor(fragCoord / hud_scale);
    vec2 hud_res = max(floor(iResolution.xy / hud_scale), vec2(1.0));
    float hud_band = rect_fill(cell, vec2(0.0, 0.0), vec2(hud_res.x - 1.0, 16.0));
    color = mix(color, vec3(0.0), hud_band * 0.18);

    float panel_width = floor((hud_res.x - 18.0) / 3.0);
    float group_a_x = 6.0;
    float group_b_x = 8.0 + panel_width;
    float group_c_x = 10.0 + panel_width * 2.0;

    float underline_a = rect_fill(cell, vec2(group_a_x, 15.0), vec2(group_a_x + panel_width - 8.0, 15.0));
    float underline_b = rect_fill(cell, vec2(group_b_x, 15.0), vec2(group_b_x + panel_width - 8.0, 15.0));
    float underline_c = rect_fill(cell, vec2(group_c_x, 15.0), vec2(group_c_x + panel_width - 8.0, 15.0));

    float left_prefix = 0.0;
    left_prefix += draw_char(cell, vec2(group_a_x, 2.0), 1);
    left_prefix += draw_char(cell, vec2(group_a_x + 4.0, 2.0), GLYPH_U);
    left_prefix += draw_char(cell, vec2(group_a_x + 8.0, 2.0), GLYPH_P);
    float left_score_digits = draw_seven_seg_number(cell, vec2(group_a_x + 16.0, 4.0), total_score, 6);

    float mid_prefix = 0.0;
    mid_prefix += draw_char(cell, vec2(group_b_x, 2.0), GLYPH_H);
    mid_prefix += draw_char(cell, vec2(group_b_x + 4.0, 2.0), GLYPH_I);
    mid_prefix += draw_char(cell, vec2(group_b_x + 8.0, 2.0), GLYPH_T);
    float mid_score_digits = draw_seven_seg_number(cell, vec2(group_b_x + 16.0, 4.0), hit_score, 6);

    float right_prefix = 0.0;
    right_prefix += draw_char(cell, vec2(group_c_x, 2.0), GLYPH_B);
    right_prefix += draw_char(cell, vec2(group_c_x + 4.0, 2.0), GLYPH_O);
    right_prefix += draw_char(cell, vec2(group_c_x + 8.0, 2.0), GLYPH_N);
    float right_score_digits = draw_seven_seg_number(cell, vec2(group_c_x + 16.0, 4.0), bonus_score, 6);

    color += vec3(1.0, 0.46, 0.24) * (left_prefix + left_score_digits + underline_a);
    color += vec3(0.22, 0.95, 1.0) * (mid_prefix + mid_score_digits + underline_b);
    color += vec3(1.0, 0.92, 0.35) * (right_prefix + right_score_digits + underline_c);

    float scanline = 1.0 - 0.04 * mod(fragCoord.y, 2.0);
    float dither = (mod(floor(fragCoord.x) + floor(fragCoord.y) * 2.0, 4.0) - 1.5) / 36.0;
    color = clamp(color + dither, 0.0, 1.0);
    color = floor(color * 7.0 + 0.5) / 7.0;
    color *= scanline;

    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
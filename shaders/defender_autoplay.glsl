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

float segment_distance(vec2 p, vec2 a, vec2 b)
{
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.0001), 0.0, 1.0);
    return length(pa - ba * h);
}

float line_mask(vec2 p, vec2 a, vec2 b, float width)
{
    return step(segment_distance(p, a, b), width);
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
    vec2 id = floor(vec2((uv.x + iTime * speed + seed) * scale, (uv.y + seed * 0.19) * scale));
    float star = step(density, hash12(id + seed * 11.0));
    float twinkle = step(0.45, hash12(id + floor(iTime * (2.0 + seed * 0.3)) + 29.0));
    return star * twinkle;
}

vec2 ship_position(float time)
{
    float ship_x = -0.50;
    float world_x = ship_x * 3.2 + time * 1.15;
    float terrain = ground_height(world_x);
    float ship_y = terrain + 0.22 + 0.04 * sin(time * 1.6) + 0.02 * sin(time * 3.4 + 0.9);
    return vec2(ship_x, ship_y);
}

vec3 render_ship(vec2 p, vec2 ship_pos, float time)
{
    float angle = 0.10 * sin(time * 1.6) + 0.05 * sin(time * 3.2 + 1.2);
    vec2 q = rotate2(-angle) * (p - ship_pos);

    float hull = 0.0;
    hull += line_mask(q, vec2(-0.11, 0.00), vec2(-0.02, 0.045), 0.008);
    hull += line_mask(q, vec2(-0.11, 0.00), vec2(-0.02, -0.045), 0.008);
    hull += line_mask(q, vec2(-0.02, 0.045), vec2(0.080, 0.00), 0.008);
    hull += line_mask(q, vec2(-0.02, -0.045), vec2(0.080, 0.00), 0.008);
    hull += line_mask(q, vec2(-0.02, 0.024), vec2(-0.02, -0.024), 0.005);
    hull += line_mask(q, vec2(-0.06, 0.000), vec2(-0.105, 0.026), 0.005);
    hull += line_mask(q, vec2(-0.06, 0.000), vec2(-0.105, -0.026), 0.005);
    hull = clamp(hull, 0.0, 1.0);

    float canopy = ellipse_mask(q - vec2(0.003, 0.0), vec2(0.018, 0.012));
    float flame_phase = fract(time * 6.0);
    float flame_length = 0.045 + 0.020 * step(0.5, flame_phase);
    float flame = 0.0;
    flame += line_mask(q, vec2(-0.112, 0.000), vec2(-0.150 - flame_length, 0.000), 0.010);
    flame += line_mask(q, vec2(-0.100, 0.012), vec2(-0.126 - flame_length * 0.5, 0.020), 0.006);
    flame += line_mask(q, vec2(-0.100, -0.012), vec2(-0.126 - flame_length * 0.5, -0.020), 0.006);
    flame = clamp(flame, 0.0, 1.0);

    vec3 color = vec3(0.18, 0.95, 1.0) * hull;
    color += vec3(1.0, 0.30, 0.70) * canopy * 0.75;
    color += mix(vec3(1.0, 0.42, 0.08), vec3(1.0, 0.92, 0.30), step(0.5, flame_phase)) * flame;
    return color;
}

float explosion_mask(vec2 p, float phase)
{
    if (phase <= 0.0)
    {
        return 0.0;
    }

    float radius = 0.018 + phase * 0.10;
    float ring = ring_mask(p, radius, 0.010);
    float spokes = step(0.82, abs(sin(atan(p.y, p.x) * 6.0 + phase * 14.0)));
    float core = step(length(p), 0.018 + (1.0 - phase) * 0.02);
    return clamp(ring * spokes + core * (1.0 - step(0.7, phase)), 0.0, 1.0);
}

vec3 render_enemy(vec2 p, vec2 enemy_pos, float seed, float time, float explode)
{
    float wobble = 0.18 * sin(time * 1.8 + seed * 5.7);
    vec2 q = rotate2(wobble) * (p - enemy_pos);

    float frame = 0.0;
    frame += line_mask(q, vec2(-0.048, 0.000), vec2(0.000, 0.032), 0.005);
    frame += line_mask(q, vec2(-0.048, 0.000), vec2(0.000, -0.032), 0.005);
    frame += line_mask(q, vec2(0.000, 0.032), vec2(0.048, 0.000), 0.005);
    frame += line_mask(q, vec2(0.000, -0.032), vec2(0.048, 0.000), 0.005);
    frame += line_mask(q, vec2(-0.020, 0.000), vec2(0.020, 0.000), 0.004);
    frame += line_mask(q, vec2(0.000, 0.032), vec2(0.000, 0.052), 0.003);
    frame = clamp(frame, 0.0, 1.0);

    float eye = ellipse_mask(q, vec2(0.012, 0.007));
    vec3 tint = palette(seed * 0.19 + time * 0.015);
    tint = mix(tint, tint.gbr, 0.25);

    vec3 color = tint * frame * (1.0 - explode);
    color += vec3(1.0, 0.90, 0.30) * eye * step(0.45, sin(time * 8.0 + seed * 11.0) * 0.5 + 0.5) * (1.0 - explode);
    color += tint.brg * explosion_mask(p - enemy_pos, explode);
    return color;
}

vec3 render_population(vec2 p, float time)
{
    vec3 color = vec3(0.0);
    float cell_origin = floor((time * 1.15) / 0.42);

    for (int i = 0; i < 12; ++i)
    {
        float id = cell_origin + float(i) - 5.0;
        float world_x = id * 0.42 + hash11(id + 3.0) * 0.16;
        float screen_x = (world_x - time * 1.15) / 3.2;
        float terrain = ground_height(world_x);
        float tower_height = 0.035 + floor(hash11(id + 9.0) * 4.0) * 0.014;
        float tower = line_mask(p, vec2(screen_x, terrain + 0.008), vec2(screen_x, terrain + tower_height), 0.004);
        float cap = box_mask(p - vec2(screen_x, terrain + tower_height + 0.005), vec2(0.006, 0.005));
        float human_offset = mix(-0.018, 0.018, step(0.5, fract(id * 0.37)));
        float human = box_mask(p - vec2(screen_x + human_offset, terrain + 0.012), vec2(0.003, 0.005));
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

void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    float pixel_scale = max(floor(iResolution.y / 150.0), 1.0);
    vec2 raster_coord = floor(fragCoord / pixel_scale);
    vec2 raster_res = max(floor(iResolution.xy / pixel_scale), vec2(1.0));
    vec2 uv = (raster_coord + 0.5) / raster_res;
    vec2 p = (raster_coord + 0.5 - 0.5 * raster_res) / raster_res.y;
    float time = iTime;

    vec3 color = vec3(0.01, 0.01, 0.04);
    color = mix(color, vec3(0.01, 0.05, 0.12), step(0.32, uv.y));
    color = mix(color, vec3(0.02, 0.10, 0.22), step(0.68, uv.y));
    color += vec3(1.0, 0.22, 0.56) * step(abs(p.y - 0.01), 0.006);

    float stars_a = star_layer(uv + vec2(0.0, 0.00), 46.0, 0.05, 0.982, 3.0);
    float stars_b = star_layer(uv + vec2(0.0, 0.11), 62.0, 0.09, 0.988, 9.0);
    float stars_c = star_layer(uv + vec2(0.0, 0.19), 78.0, 0.15, 0.992, 15.0);
    color += vec3(0.70, 0.90, 1.0) * stars_a;
    color += vec3(1.0, 0.55, 0.80) * stars_b;
    color += vec3(1.0, 0.90, 0.35) * stars_c;

    float far_ridge = ridge_far(p.x * 2.0 + time * 0.10);
    float mid_ridge = ridge_mid(p.x * 2.5 + time * 0.18);
    color = mix(color, vec3(0.04, 0.02, 0.08), step(p.y, far_ridge));
    color += vec3(0.78, 0.28, 0.92) * step(abs(p.y - far_ridge), 0.008);
    color = mix(color, vec3(0.02, 0.08, 0.10), step(p.y, mid_ridge));
    color += vec3(0.10, 0.86, 1.0) * step(abs(p.y - mid_ridge), 0.008);

    float world_x = p.x * 3.2 + time * 1.15;
    float ground = ground_height(world_x);
    vec3 ground_line_color = mix(palette(world_x * 0.03 + 0.22), vec3(0.24, 1.0, 0.55), 0.50);
    color = mix(color, vec3(0.02, 0.08, 0.03), step(p.y, ground));
    color += ground_line_color * step(abs(p.y - ground), 0.009);
    color += render_population(p, time);

    vec2 ship_pos = ship_position(time);
    color += render_ship(p, ship_pos, time);

    float current_score = 1250.0;
    for (int i = 0; i < 6; ++i)
    {
        float fi = float(i);
        float seed = fi + 1.0;
        float cycle = fract(time * (0.18 + fi * 0.015) + seed * 0.173);
        float enemy_x = mix(1.12, -1.14, cycle);
        float enemy_y = 0.10 + 0.22 * sin(time * (0.95 + fi * 0.12) + seed * 1.7) + 0.07 * sin(time * 2.2 + seed * 2.7);
        vec2 enemy_pos = vec2(enemy_x, enemy_y);

        float shot_rate = 0.25 + fi * 0.018;
        float shot_phase = fract(time * shot_rate + seed * 0.21);
        float beam = step(abs(shot_phase - 0.72), 0.06);
        float explode = 0.0;
        if (shot_phase >= 0.74 && shot_phase < 0.92)
        {
            explode = (shot_phase - 0.74) / 0.18;
        }

        float kills = max(floor(time * shot_rate + seed * 0.21 - 0.74) + 1.0, 0.0);
        current_score += kills * (150.0 + fi * 25.0);

        vec3 beam_color = mix(vec3(0.16, 0.92, 1.0), vec3(1.0, 0.90, 0.22), hash11(seed + 8.0));
        color += render_enemy(p, enemy_pos, seed, time, explode);
        color += beam_color * line_mask(p, ship_pos + vec2(0.065, 0.0), enemy_pos, 0.006) * beam;
        color += beam_color * box_mask(p - (ship_pos + vec2(0.070, 0.0)), vec2(0.006, 0.006)) * beam;
    }

    float zone = floor(time / 18.0) + 1.0;
    float high_score = max(24000.0, current_score + 3650.0);

    vec2 cell = raster_coord;
    float hud_bg = rect_fill(cell, vec2(0.0, 0.0), vec2(raster_res.x - 1.0, 16.0));
    float hud_frame = rect_outline(cell, vec2(1.0, 1.0), vec2(raster_res.x - 2.0, 15.0));
    color = mix(color, vec3(0.00, 0.02, 0.06), hud_bg);
    color += vec3(0.10, 0.58, 1.0) * hud_frame;

    float left_label = 0.0;
    left_label += draw_char(cell, vec2(5.0, 5.0), 1);
    left_label += draw_char(cell, vec2(9.0, 5.0), GLYPH_U);
    left_label += draw_char(cell, vec2(13.0, 5.0), GLYPH_P);
    float left_score = draw_number(cell, vec2(19.0, 5.0), current_score, 6);

    float hi_label = 0.0;
    hi_label += draw_char(cell, vec2(100.0, 5.0), GLYPH_H);
    hi_label += draw_char(cell, vec2(104.0, 5.0), GLYPH_I);
    float hi_score_digits = draw_number(cell, vec2(112.0, 5.0), high_score, 6);

    float zone_label = 0.0;
    zone_label += draw_char(cell, vec2(196.0, 5.0), GLYPH_Z);
    zone_label += draw_char(cell, vec2(200.0, 5.0), GLYPH_O);
    zone_label += draw_char(cell, vec2(204.0, 5.0), GLYPH_N);
    zone_label += draw_char(cell, vec2(208.0, 5.0), GLYPH_E);
    float zone_digits = draw_number(cell, vec2(216.0, 5.0), zone, 2);

    color += vec3(1.0, 0.35, 0.25) * step(0.5, left_label);
    color += vec3(1.0, 0.82, 0.35) * step(0.5, left_score);
    color += vec3(0.15, 0.92, 1.0) * step(0.5, hi_label);
    color += vec3(0.95, 0.95, 1.0) * step(0.5, hi_score_digits);
    color += vec3(1.0, 0.92, 0.35) * step(0.5, zone_label + zone_digits);

    float scanline = 1.0 - 0.08 * mod(fragCoord.y, 2.0);
    float dither = (mod(raster_coord.x + raster_coord.y * 2.0, 4.0) - 1.5) / 28.0;
    color = clamp(color + dither, 0.0, 1.0);
    color = floor(color * 6.0 + 0.5) / 6.0;
    color *= scanline;

    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
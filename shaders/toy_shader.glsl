#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;

#define TOP_U 0.05
#define TOP_V 0.05
#define BOTTOM_U 0.30
#define BOTTOM_V 0.15
#define SHIFT_AMOUNT 0.15
#define SHIFT_SPEED 0.25
#define SHIFT_WIDTH 30.0

const mat3 eigenvapor =
    mat3(0.0, 0.70710678, 0.11547005, 0.70710678, 0.0, 0.11547005, 0.70710678, 0.70710678, -0.11547005);

float rect_mask(vec2 uv, vec2 min_corner, vec2 max_corner)
{
    vec2 inside = step(min_corner, uv) * step(uv, max_corner);
    return inside.x * inside.y;
}

float stripe_mask(float value, float count, float duty)
{
    return step(fract(value * count), duty);
}

float logo_mask(vec2 original_uv)
{
    vec2 uv = vec2(original_uv.x, 1.0 - original_uv.y);
    if (uv.x < TOP_U || uv.x > BOTTOM_U || uv.y < TOP_V || uv.y > BOTTOM_V)
    {
        return 0.0;
    }

    vec2 local = vec2((uv.x - TOP_U) / (BOTTOM_U - TOP_U), (uv.y - TOP_V) / (BOTTOM_V - TOP_V));
    float mask = 0.0;

    mask = max(mask, rect_mask(local, vec2(0.00, 0.00), vec2(0.16, 1.00)));
    mask = max(mask, rect_mask(local, vec2(0.00, 0.00), vec2(0.52, 0.16)));
    mask = max(mask, rect_mask(local, vec2(0.00, 0.42), vec2(0.48, 0.58)));
    mask = max(mask, rect_mask(local, vec2(0.00, 0.84), vec2(0.60, 1.00)));
    mask = max(mask, rect_mask(local, vec2(0.62, 0.00), vec2(0.78, 1.00)));
    mask = max(mask, rect_mask(local, vec2(0.62, 0.84), vec2(1.00, 1.00)));
    mask = max(mask, rect_mask(local, vec2(0.84, 0.00), vec2(1.00, 1.00)));

    float digital_breakup = stripe_mask(local.x + local.y * 0.13, 42.0, 0.72);
    float fine_rows = stripe_mask(local.y, 14.0, 0.82);
    return mask * digital_breakup * fine_rows;
}

void main()
{
    vec2 original_uv = v_texcoord;
    vec2 shifted_uv = original_uv;

    float scan_start = mod(SHIFT_SPEED * u_time, 1.0);
    float distance_in_scan = shifted_uv.y - scan_start;
    float in_scan = step(scan_start, shifted_uv.y);
    float scan_offset =
        in_scan * sin(2.0 * SHIFT_WIDTH * distance_in_scan + radians(45.0)) * exp(1.0 - SHIFT_WIDTH * distance_in_scan);
    shifted_uv.x += SHIFT_AMOUNT * scan_offset;

    vec4 base = texture2D(u_texture, shifted_uv);
    float mask = logo_mask(original_uv);
    vec4 composed = mix(base, vec4(vec3(mask), 1.0), mask);
    gl_FragColor = vec4(eigenvapor * composed.rgb, 1.0);
}
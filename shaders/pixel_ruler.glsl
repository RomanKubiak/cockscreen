precision mediump float;

varying vec2 v_texcoord;
uniform vec2 u_viewport_size;
uniform sampler2D u_texture;

float line_segment_mask(vec2 p, vec2 a, vec2 b, float half_width)
{
    vec2 pa = p - a;
    vec2 ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 0.0001), 0.0, 1.0);
    float distance = length(pa - ba * h);
    return 1.0 - smoothstep(0.0, half_width, distance);
}

float centered_tick(float coord, float spacing, float width)
{
    float d = abs(coord);
    float phase = mod(d, spacing);
    float distance = min(phase, spacing - phase);
    return 1.0 - smoothstep(0.0, width, distance);
}

float digit_segment(int digit, int segment)
{
    if (digit == 0)
    {
        return segment == 6 ? 0.0 : 1.0;
    }
    if (digit == 1)
    {
        return (segment == 1 || segment == 2) ? 1.0 : 0.0;
    }
    if (digit == 2)
    {
        return (segment == 0 || segment == 1 || segment == 6 || segment == 4 || segment == 3) ? 1.0 : 0.0;
    }
    if (digit == 3)
    {
        return (segment == 0 || segment == 1 || segment == 6 || segment == 2 || segment == 3) ? 1.0 : 0.0;
    }
    if (digit == 4)
    {
        return (segment == 5 || segment == 6 || segment == 1 || segment == 2) ? 1.0 : 0.0;
    }
    if (digit == 5)
    {
        return (segment == 0 || segment == 5 || segment == 6 || segment == 2 || segment == 3) ? 1.0 : 0.0;
    }
    if (digit == 6)
    {
        return (segment == 0 || segment == 5 || segment == 6 || segment == 4 || segment == 2 || segment == 3) ? 1.0
                                                                                                                : 0.0;
    }
    if (digit == 7)
    {
        return (segment == 0 || segment == 1 || segment == 2) ? 1.0 : 0.0;
    }
    if (digit == 8)
    {
        return 1.0;
    }
    if (digit == 9)
    {
        return (segment == 0 || segment == 1 || segment == 2 || segment == 3 || segment == 5 || segment == 6) ? 1.0
                                                                                                                : 0.0;
    }

    return 0.0;
}

float digit_mask(int digit, vec2 local)
{
    vec2 size = vec2(16.0, 24.0);
    vec2 p = local;
    if (p.x < 0.0 || p.y < 0.0 || p.x > size.x || p.y > size.y)
    {
        return 0.0;
    }

    float w = 1.25;
    float top = line_segment_mask(p, vec2(3.0, 2.0), vec2(13.0, 2.0), w);
    float upper_right = line_segment_mask(p, vec2(13.0, 2.0), vec2(13.0, 11.0), w);
    float lower_right = line_segment_mask(p, vec2(13.0, 11.0), vec2(13.0, 22.0), w);
    float bottom = line_segment_mask(p, vec2(3.0, 22.0), vec2(13.0, 22.0), w);
    float lower_left = line_segment_mask(p, vec2(3.0, 11.0), vec2(3.0, 22.0), w);
    float upper_left = line_segment_mask(p, vec2(3.0, 2.0), vec2(3.0, 11.0), w);
    float middle = line_segment_mask(p, vec2(3.0, 12.0), vec2(13.0, 12.0), w);

    float mask = 0.0;
    mask = max(mask, digit_segment(digit, 0) * top);
    mask = max(mask, digit_segment(digit, 1) * upper_right);
    mask = max(mask, digit_segment(digit, 2) * lower_right);
    mask = max(mask, digit_segment(digit, 3) * bottom);
    mask = max(mask, digit_segment(digit, 4) * lower_left);
    mask = max(mask, digit_segment(digit, 5) * upper_left);
    mask = max(mask, digit_segment(digit, 6) * middle);
    return mask;
}

float minus_mask(vec2 local)
{
    return line_segment_mask(local, vec2(3.0, 12.0), vec2(13.0, 12.0), 1.25);
}

float draw_100_label(vec2 frag_coord, vec2 origin, int hundreds, int negative)
{
    float mask = 0.0;
    vec2 cursor = origin;

    if (negative == 1)
    {
        mask = max(mask, minus_mask(frag_coord - cursor));
        cursor += vec2(18.0, 0.0);
    }

    mask = max(mask, digit_mask(hundreds, frag_coord - cursor));
    cursor += vec2(18.0, 0.0);
    mask = max(mask, digit_mask(0, frag_coord - cursor));
    cursor += vec2(18.0, 0.0);
    mask = max(mask, digit_mask(0, frag_coord - cursor));

    return mask;
}

float draw_zero_label(vec2 frag_coord, vec2 origin)
{
    return digit_mask(0, frag_coord - origin);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec2 pixel = gl_FragCoord.xy - 0.5 * u_viewport_size;
    vec2 screen = gl_FragCoord.xy;

    float axis_thickness = 1.35;
    float tick_minor_len = 11.0;
    float tick_major_len = 22.0;

    float axis_x = 1.0 - smoothstep(0.0, axis_thickness, abs(pixel.y));
    float axis_y = 1.0 - smoothstep(0.0, axis_thickness, abs(pixel.x));

    float minor_x = centered_tick(pixel.x, 10.0, 0.75) * step(abs(pixel.y), tick_minor_len);
    float major_x = centered_tick(pixel.x, 100.0, 0.95) * step(abs(pixel.y), tick_major_len);
    float minor_y = centered_tick(pixel.y, 10.0, 0.75) * step(abs(pixel.x), tick_minor_len);
    float major_y = centered_tick(pixel.y, 100.0, 0.95) * step(abs(pixel.x), tick_major_len);

    float ruler_mask = max(max(axis_x, axis_y), max(max(minor_x, major_x), max(minor_y, major_y)));

    vec3 color = base.rgb;
    color = mix(color, vec3(1.0, 0.92, 0.40), axis_x);
    color = mix(color, vec3(0.45, 0.85, 1.0), axis_y);
    color = mix(color, vec3(0.30, 0.75, 1.0), minor_x);
    color = mix(color, vec3(1.0, 0.85, 1.0), major_x);
    color = mix(color, vec3(0.30, 0.75, 1.0), minor_y);
    color = mix(color, vec3(1.0, 0.85, 1.0), major_y);

    float label_mask = 0.0;
    label_mask = max(label_mask, draw_zero_label(screen, 0.5 * u_viewport_size + vec2(-8.0, 30.0)));

    for (int i = 1; i <= 5; ++i)
    {
        float distance = float(i) * 100.0;
        vec2 positive_origin = 0.5 * u_viewport_size + vec2(distance - 18.0, 30.0);
        vec2 negative_origin = 0.5 * u_viewport_size + vec2(-distance - 54.0, -40.0);
        label_mask = max(label_mask, draw_100_label(screen, positive_origin, i, 0));
        label_mask = max(label_mask, draw_100_label(screen, negative_origin, i, 1));
    }

    color = mix(color, vec3(1.0), label_mask);

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), max(base.a, max(ruler_mask, label_mask)));
}
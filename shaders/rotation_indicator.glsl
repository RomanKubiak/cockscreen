/*
 * rotation_indicator.glsl
 *
 * Passes the video through unchanged and overlays a visible orientation frame:
 *   - A coloured border (green sides, red top, blue bottom) so rotation
 *     direction is immediately obvious.
 *   - A filled triangle "arrow" pointing upward from the top-centre, also
 *     red, acting as an "UP" indicator that moves with the quad.
 *
 * No custom uniforms – works out of the box on any video layer.
 */

precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;

/* Border thickness in UV space (1.0 = full quad). */
const float kBorder = 0.025;

/* Arrow triangle tip at top-centre: occupies roughly the top 8 % of the quad. */
const float kArrowTipY = 0.0; /* v = 0 is the top edge */
const float kArrowBaseY = 0.08;
const float kArrowHalfW = 0.07;

float in_border(vec2 uv)
{
    float b = kBorder;
    float inner_left = b;
    float inner_right = 1.0 - b;
    float inner_top = b;
    float inner_bottom = 1.0 - b;

    /* 1.0 if we are inside the quad but outside the inner image area */
    if (uv.x < inner_left || uv.x > inner_right || uv.y < inner_top || uv.y > inner_bottom)
    {
        return 1.0;
    }
    return 0.0;
}

/* Returns 1.0 if the point is inside the upward-pointing arrow triangle. */
float in_arrow(vec2 uv)
{
    float cx = 0.5;
    float tip_y = kArrowTipY + kBorder;
    float base_y = kArrowBaseY + kBorder;

    /* Point-in-triangle test (barycentric signs). */
    vec2 p = uv;
    vec2 a = vec2(cx, tip_y);
    vec2 b = vec2(cx - kArrowHalfW, base_y);
    vec2 c = vec2(cx + kArrowHalfW, base_y);

    float d1 = (p.x - b.x) * (a.y - b.y) - (a.x - b.x) * (p.y - b.y);
    float d2 = (p.x - c.x) * (b.y - c.y) - (b.x - c.x) * (p.y - c.y);
    float d3 = (p.x - a.x) * (c.y - a.y) - (c.x - a.x) * (p.y - a.y);

    bool has_neg = (d1 < 0.0) || (d2 < 0.0) || (d3 < 0.0);
    bool has_pos = (d1 > 0.0) || (d2 > 0.0) || (d3 > 0.0);
    return (has_neg && has_pos) ? 0.0 : 1.0;
}

void main()
{
    vec2 uv = v_texcoord;
    vec4 video = texture2D(u_texture, uv);

    float border = in_border(uv);
    float arrow = in_arrow(uv);

    /* Colour the border: red top, blue bottom, green left/right sides. */
    vec3 border_color;
    if (uv.y < kBorder)
    {
        border_color = vec3(1.0, 0.15, 0.15); /* red top */
    }
    else if (uv.y > 1.0 - kBorder)
    {
        border_color = vec3(0.15, 0.35, 1.0); /* blue bottom */
    }
    else
    {
        border_color = vec3(0.15, 1.0, 0.35); /* green sides */
    }

    vec3 out_color = video.rgb;
    if (arrow > 0.5)
    {
        out_color = vec3(1.0, 0.0, 0.0); /* solid red arrow on top border */
    }
    else if (border > 0.5)
    {
        out_color = border_color;
    }

    gl_FragColor = vec4(out_color, 1.0);
}

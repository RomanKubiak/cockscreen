#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D u_texture;

// HSV chroma key — target color in HSV (0-1 each)
// When all three are zero, defaults to yellow (H≈0.157, backward compat)
uniform float u_key_h;      // hue target   0-1  (0=red, 0.157=yellow, 0.33=green, 0.66=blue)
uniform float u_key_hrange; // hue tolerance half-width   0-0.5  (default 0.08 ≈ ±29°)
uniform float u_key_spread; // edge softness  0-1  (default 0.5: outer edge = 1.5× hrange)
uniform float u_key_smin;   // minimum pixel saturation to key  0-1  (default 0.15, rejects grays)

// Convert RGB to HSV. Returns vec3(hue 0-1, saturation 0-1, value 0-1).
vec3 rgb_to_hsv(vec3 c)
{
    float cmax = max(c.r, max(c.g, c.b));
    float cmin = min(c.r, min(c.g, c.b));
    float delta = cmax - cmin;

    float h = 0.0;
    if (delta > 0.001)
    {
        if (cmax == c.r)
        {
            h = mod((c.g - c.b) / delta, 6.0) / 6.0;
        }
        else if (cmax == c.g)
        {
            h = ((c.b - c.r) / delta + 2.0) / 6.0;
        }
        else
        {
            h = ((c.r - c.g) / delta + 4.0) / 6.0;
        }
        if (h < 0.0)
        {
            h += 1.0;
        }
    }
    float s = cmax > 0.001 ? delta / cmax : 0.0;
    return vec3(h, s, cmax);
}

void main()
{
    vec4 color = texture2D(u_texture, v_texcoord);
    vec3 hsv = rgb_to_hsv(color.rgb);

    // Fall back to yellow when no custom target is set
    float has_custom = step(0.001, u_key_h + u_key_hrange);
    float target_h = mix(0.157, u_key_h, has_custom);
    float hrange = mix(0.08, u_key_hrange, step(0.001, u_key_hrange));
    float spread = mix(0.5, u_key_spread, step(0.001, u_key_spread));
    float smin = mix(0.15, u_key_smin, step(0.001, u_key_smin));

    // Circular hue distance (handles red wrap-around at 0/1)
    float hue_diff = abs(hsv.x - target_h);
    hue_diff = min(hue_diff, 1.0 - hue_diff);

    // Hue match: hard edge at hrange, soft falloff to hrange*(1+spread)
    float outer = hrange * (1.0 + spread);
    float hue_match = 1.0 - smoothstep(hrange, outer, hue_diff);

    // Saturation gate: reject pixels that are too gray
    float sat_match = smoothstep(smin - 0.04, smin + 0.04, hsv.y);

    float keyed = hue_match * sat_match;
    float alpha = color.a * (1.0 - keyed);

    gl_FragColor = vec4(color.rgb, alpha);
}

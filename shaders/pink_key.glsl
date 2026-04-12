precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_key_r;
uniform float u_key_g;
uniform float u_key_b;

float yellow_key_strength(vec3 color)
{
    vec3 default_target = vec3(0.96, 0.86, 0.12);
    vec3 mapped_target = clamp(vec3(u_key_r, u_key_g, u_key_b), 0.0, 1.0);
    float has_custom_target = step(0.001, mapped_target.r + mapped_target.g + mapped_target.b);
    vec3 target = mix(default_target, mapped_target, has_custom_target);
    vec3 diff = abs(color - target);

    float distance = diff.r * 1.05 + diff.g * 1.00 + diff.b * 1.20;
    float target_match = 1.0 - smoothstep(0.10, 0.24, distance);

    float saturation = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));
    float saturation_match = smoothstep(0.12, 0.42, saturation);

    float blue_rejection = 1.0 - smoothstep(0.12, 0.40, color.b);
    float red_match = smoothstep(0.45, 0.88, color.r);
    float green_match = smoothstep(0.45, 0.88, color.g);

    return clamp(target_match * saturation_match * blue_rejection * red_match * green_match, 0.0, 1.0);
}

void main()
{
    vec4 color = texture2D(u_texture, v_texcoord);
    float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    float saturation = max(color.r, max(color.g, color.b)) - min(color.r, min(color.g, color.b));

    float keyed_yellow = yellow_key_strength(color.rgb);
    float keyed_black = 1.0 - smoothstep(0.06, 0.20, luma);
    float keyed_bright = smoothstep(0.58, 0.92, luma) * smoothstep(0.10, 0.42, saturation);
    float keyed_blue = smoothstep(0.30, 0.72, color.b) * smoothstep(0.08, 0.55, luma);

    float keyed = clamp(max(keyed_yellow, max(keyed_black, max(keyed_bright, keyed_blue))), 0.0, 1.0);
    float alpha = color.a * (1.0 - keyed);

    gl_FragColor = vec4(color.rgb, alpha);
}

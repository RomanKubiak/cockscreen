precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform vec2 u_video_size;
uniform float u_time;
uniform float u_wire_density;

float line_mask(float value, float width)
{
    return 1.0 - smoothstep(0.0, width, abs(value));
}

float mirrored_bounce(float t, float min_value, float max_value)
{
    float range = max(max_value - min_value, 0.0001);
    float cycle = mod(t, range * 2.0);
    float mirrored = cycle < range ? cycle : 2.0 * range - cycle;
    return min_value + mirrored;
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);

    float radius = 0.24 + 0.02 * sin(u_time * 0.7);
    vec2 min_center = vec2(radius * 1.15);
    vec2 max_center = vec2(1.0 - radius * 1.15);
    vec2 center = vec2(
        mirrored_bounce(u_time * 0.23 + 0.11, min_center.x, max_center.x),
        mirrored_bounce(u_time * 0.31 + 0.37, min_center.y, max_center.y)
    );
    vec2 aspect = vec2(max(u_video_size.x, 1.0) / max(u_video_size.y, 1.0), 1.0);
    vec2 p = (v_texcoord - center) * aspect / radius;
    float r2 = dot(p, p);

    if (r2 > 1.0)
    {
        gl_FragColor = base;
        return;
    }

    float z = sqrt(max(0.0, 1.0 - r2));
    vec3 n = normalize(vec3(p, z));

    float spin = u_time * 1.45;
    mat2 twist = mat2(cos(spin), -sin(spin), sin(spin), cos(spin));
    vec2 surface = twist * vec2(atan(n.x, n.z), asin(clamp(n.y, -1.0, 1.0)));

    float density = clamp(u_wire_density, 0.0, 1.0);
    float meridians = mix(6.0, 18.0, density);
    float parallels = mix(4.0, 12.0, density);
    float line_width = mix(0.06, 0.025, density);
    float grid = max(line_mask(sin(surface.x * meridians), line_width), line_mask(sin(surface.y * parallels), line_width));

    float edge_fade = smoothstep(1.0, 0.76, sqrt(r2));
    float highlight = smoothstep(0.0, 0.65, z);
    float wire = grid * edge_fade * highlight;

    vec3 wire_color = vec3(0.95, 0.98, 1.0);
    vec3 color = clamp(base.rgb + wire_color * wire * 1.25, 0.0, 1.0);

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}
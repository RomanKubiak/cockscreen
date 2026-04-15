precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;

float line_mask(float coord, float half_width)
{
    float phase = fract(coord);
    float distance_to_line = min(phase, 1.0 - phase);
    return 1.0 - step(half_width, distance_to_line);
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);

    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    vec2 uv = v_texcoord;
    float screen_y = 1.0 - uv.y;
    float horizon = 0.50;
    float top_edge_y = horizon + 0.075;

    if (screen_y <= horizon)
    {
        gl_FragColor = base;
        return;
    }

    float centered_x = (uv.x - 0.5) * aspect;

    float perspective = 1.0 / max(screen_y - horizon, 0.035);
    float world_x = centered_x * perspective * 0.22;
    float line_width = 0.0045;
    float horizontal_line_width = line_width * 0.2;
    float top_line_width = horizontal_line_width * 1.6;
    float plane_mask = step(top_edge_y + top_line_width * 1.2, screen_y);
    float travel = u_time * 0.12;

    float vertical_minor = line_mask(world_x * 8.0, line_width) * plane_mask;
    float vertical_major = line_mask(world_x * 2.0, line_width) * plane_mask;
    float horizontal = 0.0;

    for (int i = 0; i < 11; ++i)
    {
        float world_z = 0.42 + mod(float(i) * 0.38 - travel, 11.0 * 0.38);
        float projected_y = horizon + 0.30 / world_z;
        float visible = step(top_edge_y + top_line_width * 1.2, projected_y) * step(projected_y, 1.0);
        float line = 1.0 - step(horizontal_line_width, abs(screen_y - projected_y));
        horizontal = max(horizontal, line * visible);
    }

    float top_line = 1.0 - step(top_line_width, abs(screen_y - top_edge_y));

    float grid = max(vertical_minor, vertical_major);
    grid = max(grid, horizontal * plane_mask);
    grid = max(grid, top_line);

    vec3 composed = max(base.rgb, vec3(grid));
    gl_FragColor = vec4(composed, base.a);
}
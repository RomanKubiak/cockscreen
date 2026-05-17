precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_plane_base_speed;
uniform vec2 u_resolution;

float line_mask(float coord, float half_width)
{
    float phase = fract(coord);
    float distance_to_line = min(phase, 1.0 - phase);
    return 1.0 - step(half_width, distance_to_line);
}

float segment_mask(vec2 uv, vec2 start_point, vec2 end_point, float half_width)
{
    vec2 segment = end_point - start_point;
    vec2 offset = uv - start_point;
    float length_squared = max(dot(segment, segment), 0.00001);
    float along = clamp(dot(offset, segment) / length_squared, 0.0, 1.0);
    float distance_to_line = length(offset - segment * along);
    return 1.0 - smoothstep(half_width, half_width * 2.0, distance_to_line);
}

float autonomous_speed_scale()
{
    return 0.92 + 0.10 * sin(u_time * 0.13 + 0.3) + 0.06 * sin(u_time * 0.31 + 1.2);
}

float autonomous_turn()
{
    return 0.34 * sin(u_time * 0.11 + 0.4) + 0.18 * sin(u_time * 0.047 + 2.1);
}

float autonomous_drift()
{
    return 0.11 * sin(u_time * 0.19 + 1.0) + 0.05 * sin(u_time * 0.41 + 2.6);
}

float hash2(float a, float b)
{
    return fract(sin(a * 127.1 + b * 311.7) * 43758.5453);
}

void main()
{
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    vec2 uv = v_texcoord;
    float screen_y = 1.0 - uv.y;
    float horizon = 0.50;
    float top_edge_y = horizon + 0.075;

    if (screen_y <= horizon)
    {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 0.0);
        return;
    }

    float centered_x = (uv.x - 0.5) * aspect;

    float perspective = 1.0 / max(screen_y - horizon, 0.035);
    float world_x = centered_x * perspective * 0.22;
    float line_width = 0.0045;
    float horizontal_line_width = line_width * 0.2;
    float top_line_width = horizontal_line_width * 1.6;
    float plane_mask = step(top_edge_y + top_line_width * 1.2, screen_y);
    float base_speed = mix(0.15, u_plane_base_speed, step(0.0001, u_plane_base_speed));
    float speed = max(base_speed, 0.001) * autonomous_speed_scale() * 4.0;
    float travel = u_time * speed;
    float turn_amount = autonomous_turn();
    float drift_amount = autonomous_drift();
    float camera_x = drift_amount + turn_amount * 0.22;
    float shifted_world_x = world_x - camera_x;

    float vertical_minor = line_mask(shifted_world_x * 8.0, line_width) * plane_mask;
    float vertical_major = line_mask(shifted_world_x * 2.0, line_width) * plane_mask;
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

    float motion_glow = 0.16 + 0.08 * sin(u_time * 0.27 + 0.4) + 0.05 * abs(turn_amount);

    // Per-cell fill: each major-grid rectangle independently blinks on/off
    float world_z_frag = 0.30 * perspective;
    float row_index = floor((world_z_frag - 0.42 + travel) / 0.38);
    float col_index = floor(shifted_world_x * 2.0);
    float cell_period = 4.0 + 8.0 * hash2(col_index + 77.3, row_index + 31.9);
    float cell_phase = hash2(col_index, row_index) * cell_period;
    float cell_t = mod(u_time + cell_phase, cell_period) / cell_period;
    float is_filled = step(0.60, cell_t) * plane_mask;
    float fill_brightness = 0.65 + 0.15 * sin(u_time * 0.7 + hash2(col_index + 1.1, row_index) * 6.28);
    vec3 fill_color = vec3(1.0, 0.10 + motion_glow * 0.28, 0.05) * fill_brightness * is_filled;

    vec3 wireframe_color = vec3(1.0, 0.10 + motion_glow * 0.28, 0.05) * grid * (1.0 + motion_glow * 0.24);
    float plane_alpha = clamp(grid + is_filled, 0.0, 1.0);
    vec3 plane_color = clamp(wireframe_color + fill_color, 0.0, 1.0);
    gl_FragColor = vec4(plane_color, plane_alpha);
}

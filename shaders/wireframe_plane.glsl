precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_beat;
uniform float u_audio_bpm;
uniform float u_audio_fft[16];
uniform float u_plane_base_speed;
uniform float u_plane_audio_speed_mod;
uniform float u_plane_beat_speed_mod;
uniform float u_plane_bpm_speed_scale;
uniform float u_plane_x_tone_speed_mod;
uniform float u_plane_x_tone_bias;
uniform vec2 u_resolution;

float line_mask(float coord, float half_width)
{
    float phase = fract(coord);
    float distance_to_line = min(phase, 1.0 - phase);
    return 1.0 - step(half_width, distance_to_line);
}

float audio_speed_drive()
{
    float bass = u_audio_fft[0] * 0.50 + u_audio_fft[1] * 0.30 + u_audio_fft[2] * 0.20;
    return clamp(u_audio_rms * 1.35 + u_audio_peak * 0.55 + bass * 0.80, 0.0, 1.0);
}

float beat_drive()
{
    float transient = max(u_audio_peak - u_audio_rms * 0.65, 0.0);
    float low = u_audio_fft[0] * 0.60 + u_audio_fft[1] * 0.28 + u_audio_fft[2] * 0.12;
    float inferred = smoothstep(0.07, 0.34, transient * 1.25 + low * 0.80);
    return max(clamp(u_audio_beat, 0.0, 1.0), inferred);
}

float bpm_speed_drive()
{
    float bpm = clamp(u_audio_bpm, 0.0, 240.0);
    if (bpm <= 0.0)
    {
        return 1.0 + beat_drive() * 0.35;
    }

    return clamp(bpm / 120.0, 0.35, 2.0);
}

float low_tone_energy()
{
    return clamp(
        u_audio_fft[0] * 0.18 +
        u_audio_fft[1] * 0.18 +
        u_audio_fft[2] * 0.18 +
        u_audio_fft[3] * 0.16 +
        u_audio_fft[4] * 0.15 +
        u_audio_fft[5] * 0.15,
        0.0, 1.0);
}

float high_tone_energy()
{
    return clamp(
        u_audio_fft[6] * 0.14 +
        u_audio_fft[7] * 0.14 +
        u_audio_fft[8] * 0.12 +
        u_audio_fft[9] * 0.12 +
        u_audio_fft[10] * 0.10 +
        u_audio_fft[11] * 0.10 +
        u_audio_fft[12] * 0.08 +
        u_audio_fft[13] * 0.08 +
        u_audio_fft[14] * 0.06 +
        u_audio_fft[15] * 0.06,
        0.0, 1.0);
}

float tone_x_direction()
{
    float low = low_tone_energy();
    float high = high_tone_energy();
    float direction = (high - low) / max(high + low, 0.001);
    return clamp(direction + u_plane_x_tone_bias, -1.0, 1.0);
}

float modulation_amount(float value, float fallback)
{
    return value < -0.5 ? 0.0 : mix(fallback, value, step(0.0001, value));
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
    float base_speed = mix(0.15, u_plane_base_speed, step(0.0001, u_plane_base_speed));
    float audio_speed_mod = modulation_amount(u_plane_audio_speed_mod, 1.4);
    float beat_speed_mod = modulation_amount(u_plane_beat_speed_mod, 2.2);
    float bpm_speed_scale = modulation_amount(u_plane_bpm_speed_scale, 1.0);
    float x_tone_speed_mod = modulation_amount(u_plane_x_tone_speed_mod, 0.65);
    float speed = base_speed * bpm_speed_drive() * bpm_speed_scale * (1.0 + audio_speed_drive() * audio_speed_mod);
    float travel = u_time * speed + beat_drive() * beat_speed_mod * 0.12;
    float x_travel = u_time * speed * tone_x_direction() * x_tone_speed_mod;
    float shifted_world_x = world_x - x_travel;

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

    float beat_glow = beat_drive();
    vec3 wireframe_color = vec3(1.0, 0.08 + beat_glow * 0.18, 0.05) * grid * (1.0 + beat_glow * 0.35);
    vec3 composed = max(base.rgb, wireframe_color);
    gl_FragColor = vec4(composed, base.a);
}

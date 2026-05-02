precision mediump float;

varying vec2 v_texcoord;

uniform float u_time;
uniform vec2 u_resolution;

void main()
{
    vec2 uv = v_texcoord;
    vec2 centered = uv - 0.5;
    float aspect = u_resolution.x / max(u_resolution.y, 1.0);
    centered.x *= aspect;

    const float kTau = 6.28318530718;
    float angle = u_time * kTau;
    float radius = length(centered);
    float ring_radius = 0.28;
    float ring_half_width = 0.0045;
    float rim = 1.0 - step(ring_half_width, abs(radius - ring_radius));

    float spoke_count = 12.0;
    float spoke_angle = atan(centered.y, centered.x) - angle;
    float spoke_phase = abs(sin(spoke_angle * spoke_count * 0.5));
    float spoke_width = 0.018;
    float spoke = 1.0 - step(spoke_width, spoke_phase);
    spoke *= step(radius, ring_radius - 0.010);
    spoke *= step(0.04, radius);

    vec3 color = vec3(0.0);
    color += vec3(1.0, 0.0, 0.0) * rim;
    color += vec3(1.0) * spoke;

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform float u_audio_rms;
uniform float u_audio_peak;
uniform float u_audio_fft[16];
uniform vec2 u_resolution;

// Sphere / Pong controls — all default when uniform is 0
uniform float u_spin_speed;    // rev/sec   0-2     default 0.15
uniform float u_sphere_radius; // ball size 0-0.5   default 0.14
uniform float u_specular;      // 0-1               default 0.5
uniform float u_tilt;          // axis tilt 0-1     default 0.18

// -----------------------------------------------------------------------
// Rotate a vector around the X axis by angle (radians)
vec3 rot_x(vec3 v, float a)
{
    float c = cos(a);
    float s = sin(a);
    return vec3(v.x, c * v.y - s * v.z, s * v.y + c * v.z);
}

// Rotate a vector around the Y axis by angle (radians)
vec3 rot_y(vec3 v, float a)
{
    float c = cos(a);
    float s = sin(a);
    return vec3(c * v.x + s * v.z, v.y, -s * v.x + c * v.z);
}
// -----------------------------------------------------------------------

void main()
{
    const float PI = 3.14159265;

    // Fill defaults when uniforms are not driven
    float spin = mix(0.15, u_spin_speed, step(0.001, u_spin_speed));
    float radius = mix(0.14, u_sphere_radius, step(0.001, u_sphere_radius));
    float spec = mix(0.5, u_specular, step(0.001, u_specular));
    float tilt = mix(0.18, u_tilt, step(0.001, u_tilt));

    // Low-band audio drives a radius pulse and highlight boost
    float bass = u_audio_fft[0] * 0.5 + u_audio_fft[1] * 0.3 + u_audio_fft[2] * 0.2;
    radius += bass * 0.025;

    // Aspect-correct UV centred at (0,0)
    float aspect = u_resolution.x > 0.5 ? (u_resolution.x / u_resolution.y) : 1.778;
    vec2 uv = (v_texcoord - vec2(0.5)) * vec2(aspect, 1.0);

    // Full-viewport field bounds (in UV space)
    float fx = aspect * 0.5; // half-width  (≈ 0.889 for 16:9)
    float fy = 0.5;          // half-height

    // ------------------------------------------------------------------
    // Paddle capsule geometry — 3-D vertical capsules at the screen edges
    // pad_r  : capsule radius (controls depth and apparent width)
    // pad_hh : cylinder half-height (grows with audio RMS)
    // pad_ax : X position of the capsule axis (left: -pad_ax, right: +pad_ax)
    float pad_r = 0.038;
    float pad_hh = 0.13 + u_audio_rms * 0.07;
    float pad_ax = fx - pad_r - 0.006;
    float pad_r2 = pad_r * pad_r;

    // ------------------------------------------------------------------
    // Ball trajectory — two incommensurate triangle waves
    // The ball bounces wall-to-wall in X (paddle-to-paddle) and Y (top/bottom)
    float ball_spd = 0.28 + spin * 0.65 + bass * 0.25;
    float tx = fract(u_time * ball_spd * 0.41);
    float ty = fract(u_time * ball_spd * 0.63 + 0.27);
    // Triangle wave: fract 0→1 → reflected to go -1 → +1 → -1
    float tri_x = abs(tx * 2.0 - 1.0) * 2.0 - 1.0;
    float tri_y = abs(ty * 2.0 - 1.0) * 2.0 - 1.0;
    float max_bx = pad_ax - pad_r * 1.1 - radius; // ball edge touches paddle face
    float max_by = fy - radius - 0.01;
    vec2 ball_c = vec2(tri_x * max_bx, tri_y * max_by);

    // Paddle Y tracks the ball — small phase offset between left and right
    float pad_clamp = fy - pad_hh - pad_r * 1.5;
    float lpy = clamp(ball_c.y, -pad_clamp, pad_clamp);
    float rpy = clamp(ball_c.y + sin(u_time * 0.7) * 0.04, -pad_clamp, pad_clamp);

    // ------------------------------------------------------------------
    // Shared Blinn-Phong light — same direction as original sphere shader
    vec3 L = normalize(vec3(-0.4, 0.7, 1.0));
    vec3 hlf = normalize(L + vec3(0.0, 0.0, 1.0));
    float spec_boost = 1.0 + u_audio_peak * 1.2;

    // ------------------------------------------------------------------
    // SPHERE (ball) — same ray-sphere intersection as original, offset by ball_c
    vec2 buv = uv - ball_c;
    float R2 = radius * radius;
    float br2 = dot(buv, buv);
    float b_in = step(br2, R2);
    float bz = sqrt(max(R2 - br2, 0.0));
    vec3 N_b = vec3(buv, bz) / radius; // outward normal, unit length

    // Texture lookup: tilt + spin (spin_angle picks up a horizontal roll as ball moves)
    vec3 N_tex = rot_y(rot_x(N_b, tilt * 0.6), u_time * spin + ball_c.x * 0.9);
    float tu = 0.5 + atan(N_tex.z, N_tex.x) / (2.0 * PI);
    float tv = 0.5 - asin(clamp(N_tex.y, -1.0, 1.0)) / PI;
    vec4 tex = texture2D(u_texture, vec2(tu, tv));

    float lit_b = 0.22 + max(dot(N_b, L), 0.0) * 0.78;
    vec3 col_b = tex.rgb * lit_b + vec3(pow(max(dot(N_b, hlf), 0.0), 64.0) * spec * spec_boost);
    float edge_b = 1.0 - smoothstep(R2 * 0.86, R2, br2);
    float alp_b = tex.a * b_in * edge_b;

    // ------------------------------------------------------------------
    // LEFT PADDLE — 3-D vertical capsule, axis at x = -pad_ax
    // Ray-capsule intersection (orthographic, ray along +Z):
    //   nearest point on the capsule axis = (-pad_ax, clamp(uv.y, lpy±pad_hh), 0)
    //   XY distance from that point → determines hit and z-depth
    float l_dx = uv.x + pad_ax; // signed X offset from axis
    float l_cy = clamp(uv.y, lpy - pad_hh, lpy + pad_hh);
    float l_ey = uv.y - l_cy; // Y distance past cap (0 in body)
    float l_d2 = l_dx * l_dx + l_ey * l_ey;
    float l_hit = step(l_d2, pad_r2);
    float l_z = sqrt(max(pad_r2 - l_d2, 0.0));
    vec3 N_l = normalize(vec3(l_dx, l_ey, l_z)); // surface normal
    float fr_l = 1.0 - N_l.z;                    // fresnel rim term
    vec3 col_l = vec3(0.70, 0.80, 0.90) * (0.18 + max(dot(N_l, L), 0.0) * 0.82) +
                 vec3(pow(max(dot(N_l, hlf), 0.0), 48.0) * spec * spec_boost * 0.9) +
                 vec3(0.08, 0.12, 0.20) * fr_l * fr_l    // rim highlight
                 + vec3(0.00, 0.02, 0.05) * u_audio_rms; // audio tint

    // RIGHT PADDLE — mirror geometry at x = +pad_ax
    float r_dx = uv.x - pad_ax;
    float r_cy = clamp(uv.y, rpy - pad_hh, rpy + pad_hh);
    float r_ey = uv.y - r_cy;
    float r_d2 = r_dx * r_dx + r_ey * r_ey;
    float r_hit = step(r_d2, pad_r2);
    float r_z = sqrt(max(pad_r2 - r_d2, 0.0));
    vec3 N_r = normalize(vec3(r_dx, r_ey, r_z));
    float fr_r = 1.0 - N_r.z;
    vec3 col_r = vec3(0.70, 0.80, 0.90) * (0.18 + max(dot(N_r, L), 0.0) * 0.82) +
                 vec3(pow(max(dot(N_r, hlf), 0.0), 48.0) * spec * spec_boost * 0.9) +
                 vec3(0.08, 0.12, 0.20) * fr_r * fr_r + vec3(0.00, 0.02, 0.05) * u_audio_rms;

    // ------------------------------------------------------------------
    // Centre-line dashes — subtle Pong court reference
    float dash_m = step(0.5, fract(uv.y * 7.5)) * (1.0 - smoothstep(0.003, 0.010, abs(uv.x)));
    float dash_a = 0.20 * dash_m;
    vec3 dash_c = vec3(0.60, 0.75, 0.70);

    // ------------------------------------------------------------------
    // "Over" compositing — transparent base so underlying video shows through.
    // Layer order: dashes → left paddle → right paddle → ball (topmost).
    vec3 out_c = vec3(0.0);
    float out_a = 0.0;

    // Centre dashes (semi-transparent)
    out_c = mix(out_c, dash_c, dash_a);
    out_a = out_a + dash_a * (1.0 - out_a);

    // Left paddle (opaque where hit)
    out_c = mix(out_c, col_l, l_hit);
    out_a = out_a + l_hit * (1.0 - out_a);

    // Right paddle
    out_c = mix(out_c, col_r, r_hit);
    out_a = out_a + r_hit * (1.0 - out_a);

    // Ball (sphere with texture alpha)
    out_c = mix(out_c, col_b, alp_b);
    out_a = out_a + alp_b * (1.0 - out_a);

    gl_FragColor = vec4(out_c, out_a);
}

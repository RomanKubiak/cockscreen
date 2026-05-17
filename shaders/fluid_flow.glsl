#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform float u_time;
uniform vec2  u_viewport_size;

uniform float u_flow_speed;
uniform float u_flow_scale;
uniform float u_turbulence;
uniform float u_whirl_x;
uniform float u_whirl_y;
uniform float u_whirl_strength;
uniform float u_whirl_radius;
uniform float u_palette_shift;
uniform float u_palette_contrast;
uniform float u_color_speed;
uniform float u_brightness;

vec2 emitter1(float t) { return vec2(0.12, 0.5 + sin(t * 0.62) * 0.20); }
vec2 emitter2(float t) { return vec2(0.88, 0.5 + cos(t * 0.62 + 1.5708) * 0.20); }

// Sin-based hash — stays precise with mediump for small seeds.
float hash1(float s) { return fract(sin(s * 78.233 + 12.9898) * 43758.5453); }

void main() {
    vec2 uv   = v_texcoord;
    float T   = u_time * u_flow_speed;
    float sc  = u_flow_scale;
    float asp = u_viewport_size.x / max(u_viewport_size.y, 1.0);

    vec2 e1 = emitter1(u_time);
    vec2 e2 = emitter2(u_time);

    // ── Precompute 4 random excitation events ──────────────────────────────
    // Each slot cycles on its own period; slot_idx is kept < 97 so sin() stays
    // accurate under mediump.
    vec2  exc_pos[4];
    float exc_active[4];
    float exc_spin[4];

    for (int e = 0; e < 4; e++) {
        float ef      = float(e);
        float period  = 5.0 + ef * 1.30;               // 5.0 / 6.3 / 7.6 / 8.9 s
        float st      = u_time + ef * 1.73;             // stagger so they don't fire together
        float si      = mod(floor(st / period), 97.0);  // bounded slot index
        float phase   = fract(st / period);             // [0, 1] within cycle

        float seed    = si * 17.0 + ef * 31.0;
        exc_pos[e]    = vec2(0.1 + 0.8 * hash1(seed), 0.1 + 0.8 * hash1(seed + 5.0));
        exc_spin[e]   = sign(hash1(seed + 11.0) - 0.5);

        // Smooth pulse: ramp in [0, 0.2], hold [0.2, 0.7], ramp out [0.7, 1.0]
        exc_active[e] = smoothstep(0.0, 0.2, phase) * smoothstep(1.0, 0.7, phase);
    }

    // ── Backward advection ─────────────────────────────────────────────────
    vec2 pos = uv;
    for (int i = 0; i < 3; i++) {
        vec2 p = pos * sc;
        vec2 vel;
        vel.x = sin(p.y * 1.30 + T * 0.40) + sin(p.x * 0.90 + p.y * 1.80 + T * 0.25) * u_turbulence;
        vel.y = cos(p.x * 1.30 + T * 0.37) + cos(p.x * 1.80 + p.y * 0.90 + T * 0.22) * u_turbulence;
        vel *= 0.035;

        vec2 d1 = pos - e1; d1.x *= asp;
        vel += vec2(-d1.y, d1.x) * (0.006 / (dot(d1, d1) + 0.02));

        vec2 d2 = pos - e2; d2.x *= asp;
        vel -= vec2(-d2.y, d2.x) * (0.006 / (dot(d2, d2) + 0.02));

        // Excitation vortices — deform existing dye into new swirls
        for (int e = 0; e < 4; e++) {
            vec2 de = pos - exc_pos[e]; de.x *= asp;
            vel += vec2(-de.y, de.x) * (exc_spin[e] * exc_active[e] * 0.005 / (dot(de, de) + 0.02));
        }

        pos -= vel;
    }

    // ── Dye from permanent emitters (with fake temporal history) ───────────
    float dye = 0.0;
    for (int k = 0; k < 3; k++) {
        float past = u_time - float(k) * 0.5;
        vec2 pe1 = emitter1(past);
        vec2 pe2 = emitter2(past);
        float w  = 1.0 / (float(k) + 1.0);
        float r1 = dot(pos - pe1, pos - pe1);
        float r2 = dot(pos - pe2, pos - pe2);
        dye += w * (0.0022 / (r1 + 0.0008) + 0.0022 / (r2 + 0.0008));
    }

    // ── Dye injection at excitation sites — the visible "drops" ───────────
    for (int e = 0; e < 4; e++) {
        vec2 duv = uv - exc_pos[e]; duv.x *= asp;
        dye += exc_active[e] * 0.0014 / (dot(duv, duv) + 0.0012);
    }

    // Fine-grain modulation for wispy tendrils
    dye *= 0.70 + 0.30 * sin(pos.x * 28.0 + T * 0.60) * sin(pos.y * 32.0 + T * 0.50);
    dye  = clamp(dye, 0.0, 1.0);

    float intensity = dye * u_brightness;
    gl_FragColor = vec4(intensity, 0.0, 0.0, intensity);
}

#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform float u_time;
uniform vec2  u_viewport_size;

// Flow field
uniform float u_flow_speed;       // animation speed         default 0.4
uniform float u_flow_scale;       // spatial frequency       default 2.0
uniform float u_turbulence;       // 2nd octave weight       default 0.6

// Whirl (Rankine vortex)
uniform float u_whirl_x;          // 0–1 normalized x pos   default 0.5
uniform float u_whirl_y;          // 0–1 normalized y pos   default 0.5
uniform float u_whirl_strength;   // signed, pos=CCW         default 3.0
uniform float u_whirl_radius;     // core size, 0–1          default 0.25

// Color
uniform float u_palette_shift;    // hue / phase offset      default 0.0
uniform float u_palette_contrast; // color vividness         default 0.8
uniform float u_color_speed;      // dye drift rate          default 0.12
uniform float u_brightness;       // output gain             default 1.0

// ── Noise ───────────────────────────────────────────────────────────────────

float hash(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p.yx + 19.19);
    return fract(p.x * p.y);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Curl of a scalar noise potential → divergence-free 2D velocity field.
vec2 curl(vec2 p) {
    const float e = 0.012;
    float nx = vnoise(p + vec2(e,  0.0)) - vnoise(p - vec2(e,  0.0));
    float ny = vnoise(p + vec2(0.0, e )) - vnoise(p - vec2(0.0, e ));
    return vec2(ny, -nx) * (1.0 / (2.0 * e));
}

// ── Vortex ──────────────────────────────────────────────────────────────────

// Rankine vortex centered at u_whirl_{x,y}.  The aspect-ratio correction keeps
// the core circular on non-square displays.
vec2 vortex(vec2 uv) {
    vec2 d = uv - vec2(u_whirl_x, u_whirl_y);
    d.x *= u_viewport_size.x / u_viewport_size.y;
    float r2   = dot(d, d);
    float core = u_whirl_radius * u_whirl_radius;
    // Rankine tangential speed peaks at the core radius then falls as 1/r.
    float speed = u_whirl_strength * sqrt(r2) / (r2 + core + 0.0001);
    float r     = sqrt(r2) + 0.0001;
    return vec2(-d.y, d.x) * (speed / r);
}

// ── Palette ─────────────────────────────────────────────────────────────────

// Inigo Quilez cosine palette — aurora/plasma default look.
vec3 palette(float t) {
    t += u_palette_shift;
    vec3 a = vec3(0.5);
    vec3 b = vec3(0.5) * clamp(u_palette_contrast, 0.0, 2.0);
    vec3 c = vec3(1.0, 0.7, 0.4);
    vec3 d = vec3(0.00, 0.15, 0.30);
    return a + b * cos(6.28318 * (c * t + d));
}

// ── Main ─────────────────────────────────────────────────────────────────────

void main() {
    vec2 uv = v_texcoord;
    float T  = u_time * u_flow_speed;
    float sc = u_flow_scale;

    // Backward-advect the sample point through the combined velocity field.
    // Each step traces through curl noise (two octaves) plus the vortex.
    // Four steps gives good streaky advection with no framebuffer state needed.
    vec2 pos = uv;
    for (int i = 0; i < 4; i++) {
        vec2 p1 = pos * sc        + vec2(T * 0.11, T * 0.07);
        vec2 p2 = pos * sc * 2.3  + vec2(T * 0.13, T * 0.09) + vec2(4.1, 2.7);
        vec2 vel = curl(p1) + curl(p2) * u_turbulence;
        vel += vortex(pos);
        pos -= vel * 0.018;
    }

    // Sample two dye layers at the advected position and blend them.
    float d1 = vnoise(pos * sc * 0.8 + vec2(0.0,               T * u_color_speed * 0.3));
    float d2 = vnoise(pos * sc * 1.4 + vec2(T * u_color_speed * 0.2, 0.0) + vec2(8.3, 6.1));
    float dye = (d1 + d2 * 0.5) / 1.5;

    vec3 col = palette(dye + T * u_color_speed * 0.04) * u_brightness;
    gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}

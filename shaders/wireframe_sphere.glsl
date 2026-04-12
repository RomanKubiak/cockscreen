precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform vec2 u_viewport_size;
uniform float u_time;
uniform float u_audio_level;
uniform float u_audio_fft[16];

const float kPi = 3.14159265359;
const float kTau = 6.28318530718;

// ---- geometry helpers -------------------------------------------------------

// Side-by-side ray-march of a unit sphere centred at `centre` (NDC space).
// Returns distance to sphere surface, or -1 if the ray misses.
float sphere_hit(vec2 p, vec2 centre, float radius)
{
    vec2 d = p - centre;
    float r2 = dot(d, d);
    return r2 <= radius * radius ? sqrt(max(0.0, 1.0 - r2 / (radius * radius))) : -1.0;
}

float line_mask(float v, float w)
{
    return 1.0 - smoothstep(0.0, w, abs(v));
}

// Normalised mirrored bounce in [lo, hi] for a scalar time input.
float bounce(float t, float lo, float hi)
{
    float span = max(hi - lo, 0.001);
    float cycle = mod(t, span * 2.0);
    float m = cycle < span ? cycle : span * 2.0 - cycle;
    return lo + m;
}

// Simple 3×3 rotation matrix around Y then X axes
mat3 rot_yx(float ay, float ax)
{
    float sy = sin(ay), cy = cos(ay);
    float sx = sin(ax), cx = cos(ax);
    return mat3(cy, 0.0, sy, sy * sx, cx, -cy * sx, -sy * cx, sx, cy * cx);
}

// ---- main -------------------------------------------------------------------

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);

    vec2 aspect = vec2(u_viewport_size.x / max(u_viewport_size.y, 1.0), 1.0);
    vec2 uv = (v_texcoord - 0.5) * aspect * 2.0; // -aspect..+aspect

    // Audio bands
    float low = (u_audio_fft[0] + u_audio_fft[1] + u_audio_fft[2] + u_audio_fft[3]) * 0.25;
    float high = (u_audio_fft[12] + u_audio_fft[13] + u_audio_fft[14] + u_audio_fft[15]) * 0.25;
    float presence = clamp(u_audio_level * 1.4, 0.0, 1.0);

    // Sphere radius — gently pulses with bass
    float radius = 0.26 + 0.04 * low + 0.015 * sin(u_time * 1.1);

    // Bounce margin (centre must stay inside viewport so sphere stays visible)
    float margin = radius * 1.08;
    float x_range = aspect.x - margin; // half-NDC width
    float y_range = 1.0 - margin;

    // Bouncing centre in NDC space
    vec2 centre = vec2(bounce(u_time * (0.19 + low * 0.09), -x_range, x_range),
                       bounce(u_time * (0.27 + high * 0.07), -y_range, y_range));

    // --- hit test ------------------------------------------------------------
    vec2 d_raw = uv - centre;
    float d2 = dot(d_raw, d_raw);
    float r2 = radius * radius;

    if (d2 > r2 * 1.0001)
    {
        gl_FragColor = base;
        return;
    }

    float z = sqrt(max(0.0, 1.0 - d2 / r2));
    vec3 n = normalize(vec3(d_raw / radius, z));

    // --- spin ----------------------------------------------------------------
    float spin_y = u_time * (0.55 + low * 0.7);
    float spin_x = u_time * (0.28 + high * 0.5);
    vec3 ns = rot_yx(spin_y, spin_x) * n;

    // --- wireframe grid (lat/lon) -------------------------------------------
    float lon = atan(ns.x, ns.z);             // -pi .. +pi
    float lat = asin(clamp(ns.y, -1.0, 1.0)); // -pi/2 .. +pi/2

    const float kMeridians = 12.0;
    const float kParallels = 8.0;
    const float kLineWidth = 0.19;
    const float kBorderWidth = 0.28; // dark halo around each line

    float meridian_mask = line_mask(sin(lon * kMeridians * 0.5), kLineWidth);
    float parallel_mask = line_mask(sin(lat * kParallels), kLineWidth);
    float grid = max(meridian_mask, parallel_mask);
    float border_mask =
        max(line_mask(sin(lon * kMeridians * 0.5), kBorderWidth), line_mask(sin(lat * kParallels), kBorderWidth));
    // dark halo makes lines pop against any background
    float border = clamp(border_mask - grid, 0.0, 1.0);

    // --- colour --------------------------------------------------------------
    vec3 wire_color = vec3(1.0); // white
    vec3 fill_color = vec3(0.0); // black interior

    // Dim lines that face away from viewer
    float lit = 0.18 + 0.82 * smoothstep(0.0, 1.0, z);
    float rim_fade = 1.0 - smoothstep(0.82, 1.0, sqrt(d2 / r2));

    vec3 color = fill_color;
    color = mix(color, vec3(0.0), border * rim_fade * 0.75); // dark halo
    color = mix(color, wire_color * lit * rim_fade * (0.7 + presence * 0.3), grid * 0.98);

    // Subtle specular highlight
    vec3 light_dir = normalize(vec3(0.5, 0.8, 1.0));
    float spec = pow(max(dot(ns, light_dir), 0.0), 18.0);
    color += vec3(spec * 0.18 * (0.5 + presence * 0.5));

    // Blend over background
    float alpha = clamp(grid * rim_fade, 0.0, 1.0);
    color = mix(base.rgb, color, alpha * 0.97 + 0.02);

    gl_FragColor = vec4(clamp(color, 0.0, 1.0), base.a);
}

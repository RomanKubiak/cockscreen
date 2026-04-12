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

// Sphere controls — all default when uniform is 0
uniform float u_spin_speed;    // rev/sec   0-2     default 0.15
uniform float u_sphere_radius; // 0-0.5     default 0.38
uniform float u_specular;      // 0-1       default 0.5
uniform float u_tilt;          // axis tilt 0-1     default 0.18  (X-axis lean)

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
    // Fill defaults when uniforms are not driven
    float spin = mix(0.15, u_spin_speed, step(0.001, u_spin_speed));
    float radius = mix(0.38, u_sphere_radius, step(0.001, u_sphere_radius));
    float spec = mix(0.5, u_specular, step(0.001, u_specular));
    float tilt = mix(0.18, u_tilt, step(0.001, u_tilt));

    // Low-band audio drives a radius pulse and highlight boost
    float bass = u_audio_fft[0] * 0.5 + u_audio_fft[1] * 0.3 + u_audio_fft[2] * 0.2;
    radius += bass * 0.035;

    // Aspect-correct UV centred at (0,0)
    float aspect = u_resolution.x > 0.5 ? (u_resolution.x / u_resolution.y) : 1.778;
    vec2 uv = (v_texcoord - vec2(0.5)) * vec2(aspect, 1.0);

    // ------------------------------------------------------------------
    // Ray–sphere intersection (orthographic projection, ray along +Z)
    // Sphere centre (0,0,0), radius R
    float R2 = radius * radius;
    float r2 = dot(uv, uv);
    float inside = step(r2, R2); // 1 inside sphere, 0 outside

    float z = sqrt(max(R2 - r2, 0.0));
    vec3 hit = vec3(uv, z); // point on sphere surface
    vec3 N = hit / radius;  // outward normal (unit length)

    // ------------------------------------------------------------------
    // Apply tilt around X then Y-spin to compute the texture lookup normal
    float tilt_angle = tilt * 0.6; // map 0-1 → 0-0.6 rad (~34°)
    float spin_angle = u_time * spin;

    vec3 N_tex = rot_x(N, tilt_angle);
    N_tex = rot_y(N_tex, spin_angle);

    // Spherical UV projection (equirectangular)
    const float PI = 3.14159265;
    float tu = 0.5 + atan(N_tex.z, N_tex.x) / (2.0 * PI);
    float tv = 0.5 - asin(clamp(N_tex.y, -1.0, 1.0)) / PI;

    vec4 tex = texture2D(u_texture, vec2(tu, tv));

    // ------------------------------------------------------------------
    // Phong lighting — fixed upper-left key light, view along +Z
    vec3 light = normalize(vec3(-0.4, 0.7, 1.0));
    float diffuse = max(dot(N, light), 0.0);

    // Blinn-Phong half-vector
    vec3 half_v = normalize(light + vec3(0.0, 0.0, 1.0));
    float specular = pow(max(dot(N, half_v), 0.0), 64.0);

    // Audio peak flashes the specular highlight
    float spec_boost = 1.0 + u_audio_peak * 1.2;
    float lighting = 0.22 + diffuse * 0.78;

    vec3 color = tex.rgb * lighting + vec3(specular * spec * spec_boost);

    // Smooth silhouette edge
    float edge = smoothstep(R2, R2 * 0.86, r2);

    gl_FragColor = vec4(color, tex.a * edge * inside);
}

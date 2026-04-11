// Rorschach Inkblot Shader
#ifdef GL_ES
precision highp float;
#endif

varying vec2 v_texcoord;
uniform float u_time;
uniform vec2 u_viewport_size;
uniform sampler2D u_texture;

// Simple 2D Pseudo-random function
float random(vec2 st)
{
    return fract(sin(dot(st.xy, vec2(12.9898, 78.233))) * 43758.5453123);
}

// 2D Noise function
float noise(vec2 st)
{
    vec2 i = floor(st);
    vec2 f = fract(st);
    float a = random(i);
    float b = random(i + vec2(1.0, 0.0));
    float c = random(i + vec2(0.0, 1.0));
    float d = random(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

// Fractal Brownian Motion to give it "organic" edges
float fbm(vec2 st)
{
    float value = 0.0;
    float amplitude = 0.5;
    for (int i = 0; i < 6; i++)
    {
        value += amplitude * noise(st);
        st *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);

    // 1. Normalize coordinates (0.0 to 1.0)
    vec2 uv = gl_FragCoord.xy / max(u_viewport_size, vec2(1.0));

    // 2. Create Symmetry (The "Rorschach" Mirror)
    // We flip the x-coordinate around the center (0.5)
    uv.x = abs(uv.x - 0.5);

    // 3. Timing for new blot every 10 seconds
    // 'seed' changes abruptly every 10s, 'transition' can be used for fading if desired
    float cycle = floor(u_time / 10.0);

    // 4. Center the blot and scale
    vec2 pos = uv * 3.5;
    pos.y -= u_time * 0.05; // Subtle vertical drift for "growth" feel

    // 5. Generate Noise Shape
    // We add the cycle to the coordinates to get a brand new shape every 10s
    float n = fbm(pos + vec2(cycle * 10.0));

    // 6. Apply a Radial Gradient mask
    // This keeps the ink in the center of the screen and prevents edge-clipping
    float dist = distance(uv, vec2(0.0, 0.5));
    float mask = smoothstep(0.4, 0.1, dist);
    n *= mask;

    // 7. Thresholding (The "Ink" look)
    // Values above the threshold become the visible ink region.
    float ink = smoothstep(0.4, 0.45, n);

    // 8. Output only the black ink shape; everything else stays transparent.
    vec3 inkColor = vec3(0.0, 0.0, 0.0);
    vec3 color = mix(base.rgb, inkColor, ink);

    gl_FragColor = vec4(color, base.a);
}
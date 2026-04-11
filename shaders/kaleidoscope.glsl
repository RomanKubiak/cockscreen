#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform float u_time;
uniform vec2 u_viewport_size;
uniform sampler2D u_texture;

// https://iquilezles.org/articles/palettes/
vec3 palette(float t)
{
    vec3 a = vec3(0.5, 0.5, 0.5);
    vec3 b = vec3(0.5, 0.5, 0.5);
    vec3 c = vec3(1.0, 1.0, 1.0);
    vec3 d = vec3(0.263, 0.416, 0.557);

    return a + b * cos(6.28318 * (c * t + d));
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec2 uv =
        (gl_FragCoord.xy * 2.0 - u_viewport_size.xy) / max(u_viewport_size.y, 1.0); // coordinate system from center
    vec2 uv0 = uv;
    vec3 finalColor = vec3(0.0);

    for (float i = 0.0; i < 3.0; i++)
    {
        uv = fract(uv * 1.5) - 0.5;

        float d = length(uv) * exp(-length(uv0));

        vec3 col = palette(length(uv0) + i * 0.4 + u_time * 0.4);

        d = sin(d * 8.0 + u_time) / 8.0;
        d = abs(d);

        d = pow(0.01 / d, 1.2);

        finalColor += col * d;
    }

    vec3 kaleidoscope = finalColor / (1.0 + finalColor);
    vec3 color = clamp(base.rgb + kaleidoscope * 0.35, 0.0, 1.0);

    gl_FragColor = vec4(color, base.a);
}
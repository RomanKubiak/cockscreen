#define PI 3.1415926537
#define size 9.8
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    vec2 uv = (fragCoord - iResolution.xy * 0.5) / iResolution.y;
    float centerBlob = 1.0 - length(uv);
    uv /= length(uv) * 0.3 + 1.0;
    float grid = 1.0 - length(smoothstep(0.3, -0.06, abs(fract(uv * size * 0.5 * PI + 0.5) - 0.5)));
    uv += 0.01 * sin(10.0 * (uv * size));
    uv += iMouse.xy * 0.00001 * grid;
    vec3 color = texture(iChannel0, uv + iTime * vec2(0.003, 0.001)).rgb;
    color *= centerBlob * (clamp(grid, 0.0, 1.0) * 0.6 + 0.4) * (centerBlob * 2.8 + 1.0);

    fragColor = vec4(color, 1.0);
}

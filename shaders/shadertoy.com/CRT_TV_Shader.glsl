#define SCANLINES 1
#define CHROMATIC_ABERRATION 1
#define VIGNETTE 1
#define DISTORT 1

// Chromatic aberration parameters
#define CA_STRENGTH 15.0

// Vignette parameters
#define CORNER_OFFSET 0.61
#define CORNER_MASK_INTENSITY_MULT 16.0
#define BORDER_OFFSET 0.04

vec3 ChromaticAberration(vec2 uv)
{
    vec3 color = texture(iChannel0, uv).rgb;
    color.r = texture(iChannel0, (uv - 0.5) * (1.0 + CA_STRENGTH / iResolution.xy) + 0.5).r;
    color.b = texture(iChannel0, (uv - 0.5) * (1.0 - CA_STRENGTH / iResolution.xy) + 0.5).b;

    return color;
}

// https://www.imatest.com/support/docs/pre-5-2/geometric-calibration-deprecated/distortion-models/
// https://www.control.isy.liu.se/student/graduate/DynVis/Lectures/le2.pdf
// Only radial distortion. Brown Conrady gives better result and less affects performance
vec2 BrownConradyDistortion(in vec2 uv)
{
    // positive values of K1 give barrel distortion
    float k1 = 0.3;
    float k2 = 0.1;

    uv = uv * 2.0 - 1.0; // brown conrady takes [-1:1]

    float r2 = uv.x * uv.x + uv.y * uv.y;
    uv *= 1.0 + k1 * r2 + k2 * r2 * r2;

    uv = uv * 0.5 + 0.5; // [0:1]

    // using the distortion param as a scale factor, to keep the image close to the viewport dims
    float scale = abs(k1) < 1.0 ? 1.0 - abs(k1) : 1.0 / (k1 + 1.0);

    uv = uv * scale - (scale * 0.5) + 0.5; // scale from center

    return uv;
}

float Scanlines(vec2 uv)
{
    return (abs(sin(iResolution.y * uv.y)) + abs(sin(iResolution.x * uv.x))) / 2.0;
}

// Or we can bake vignette map to get more custom vignette with better visual result
// But current implementation is faster on GPU
float Vignette(vec2 uv)
{
    uv = uv - 0.5; // [-0.5; 0.5]

    // Cut the corners
    float res = length(uv) - CORNER_OFFSET;
    res *= CORNER_MASK_INTENSITY_MULT;
    res = clamp(res, 0.0, 1.0);
    res = 1.0 - res;

    // Cut the out of bounds information
    uv = abs(uv); // [0.0; 0.5]
    uv = uv - (0.5 - BORDER_OFFSET);
    uv = smoothstep(1.0, 0.0, uv / BORDER_OFFSET);

    // combine
    return min(uv.x, uv.y) * res;
}

// main shader body
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    // screen space coordinates [0;1]
    vec2 uv = fragCoord.xy / iResolution.xy;

#if DISTORT
    uv = BrownConradyDistortion(uv);
#endif

#if CHROMATIC_ABERRATION
    vec3 result = ChromaticAberration(uv);
#else
    vec3 result = texture(iChannel0, uv).rgb;
#endif

#if SCANLINES
    result *= Scanlines(uv);
#endif

#if VIGNETTE
    result *= Vignette(uv);
#endif

    // final output
    fragColor = vec4(result, 1.0);
}
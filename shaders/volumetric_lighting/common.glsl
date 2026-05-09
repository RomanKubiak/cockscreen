// #define PERFORMANCE_MODE

// ==========================
// Generic Helpers/Constants
// ==========================

// Keyboard input description: https://www.shadertoy.com/view/lsXGzf
#define KEY_A 65
#define KEY_S 83
#define KEY_D 68
#define KEY_F 70

#define PI 3.141592653589793
#define TWOPI 6.283185307179586
#define HALFPI 1.570796326794896
#define INV_SQRT_2 0.7071067811865476

#define POLAR(theta) vec3(cos(theta), 0.0, sin(theta))
#define SPHERICAL(theta, phi) (sin(phi) * POLAR(theta) + vec3(0.0, cos(phi), 0.0))

float len2Inf(vec2 v)
{
    vec2 d = abs(v);
    return max(d.x, d.y);
}

void boxClip(in vec3 boxMin, in vec3 boxMax, in vec3 p, in vec3 v, out vec2 tRange, out float didHit)
{
    // for each coord, clip tRange to only contain t-values for which p+t*v is in range
    vec3 tb0 = (boxMin - p) / v;
    vec3 tb1 = (boxMax - p) / v;
    vec3 tmin = min(tb0, tb1);
    vec3 tmax = max(tb0, tb1);

    // t must be > tRange.s and each tmin, so > max of these; similar for t1
    tRange = vec2(max(max(tmin.x, tmin.y), tmin.z), min(min(tmax.x, tmax.y), tmax.z));

    // determine whether ray intersects the box
    didHit = step(tRange.s, tRange.t);
}

// cf. Dave Hoskins https://www.shadertoy.com/view/4djSRW
// -------------------------
float hash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float hash13(vec3 p3)
{
    p3 = fract(p3 * .1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 hash31(float p)
{
    vec3 p3 = fract(vec3(p) * vec3(.1031, .1030, .0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xxy + p3.yzz) * p3.zyx);
}
// -------------------------

// cf. iq https://www.shadertoy.com/view/ll2GD3
vec3 colormap(float t)
{
    return .5 + .5 * cos(TWOPI * (t + vec3(0.0, 0.1, 0.2)));
}

vec4 blendOnto(vec4 cFront, vec4 cBehind)
{
    return cFront + (1.0 - cFront.a) * cBehind;
}

// ======================
// Voxel packing helpers
// ======================

#define BOX_MIN vec3(-1.0)
#define BOX_MAX vec3(1.0)
#define BOX_N 128.0

vec3 lmnFromWorldPos(vec3 p)
{
    vec3 uvw = (p - BOX_MIN) / (BOX_MAX - BOX_MIN);
    return uvw * vec3(BOX_N - 1.0);
}

vec3 worldPosFromLMN(vec3 lmn)
{
    return mix(BOX_MIN, BOX_MAX, lmn / (BOX_N - 1.0));
}

// Data is organized into 3 "pages" of 128x128x128 voxels.
// Each "page" takes up 2 faces of the 1024x1024 cubemap,
// each face storing 8x8=64 of the 128x128 slices.

vec3 vcubeFromLMN(in int page, in vec3 lmn)
{
    // subtexture within [0,8)^2
    float l = mod(round(lmn.x), 128.0);
    float tm = mod(l, 8.0);
    float tn = mod((l - tm) / 8.0, 8.0);
    vec2 tmn = vec2(tm, tn);

    // mn within [0,128)^2
    vec2 mn = mod(round(lmn.yz), 128.0);

    // pixel position on 1024x1024 face
    vec2 fragCoord = 128.0 * tmn + mn + 0.5;
    vec2 p = fragCoord * (2.0 / 1024.0) - 1.0;

    vec3 fv;
    if (page == 1)
    {
        fv = vec3(1.0, p);
    }
    else if (page == 2)
    {
        fv = vec3(p.x, 1.0, p.y);
    }
    else
    {
        fv = fv = vec3(p, 1.0);
    }

    if (l < 64.0)
    {
        return fv;
    }
    else
    {
        return -fv;
    }
}

void lmnFromVCube(in vec3 vcube, out int page, out vec3 lmn)
{
    // page and parity, and pixel position on 1024x1024 texture
    vec2 p;
    float parity;
    if (abs(vcube.x) > abs(vcube.y) && abs(vcube.x) > abs(vcube.z))
    {
        page = 1;
        p = vcube.yz / vcube.x;
        parity = vcube.x;
    }
    else if (abs(vcube.y) > abs(vcube.z))
    {
        page = 2;
        p = vcube.xz / vcube.y;
        parity = vcube.y;
    }
    else
    {
        page = 3;
        p = vcube.xy / vcube.z;
        parity = vcube.z;
    }
    vec2 fragCoord = floor((0.5 + 0.5 * p) * 1024.0);

    // mn within [0,128)^2
    vec2 mn = mod(fragCoord, 128.0);

    // subtexture within [0,8)^2
    vec2 tmn = floor(fragCoord / 128.0);

    float lAdd;
    if (parity > 0.0)
    {
        lAdd = 0.0;
    }
    else
    {
        lAdd = 64.0;
    }
    lmn = vec3(tmn.y * 8.0 + tmn.x + lAdd, mn);
}

// ===================
// Density definition
// ===================

#define MAX_ALPHA_PER_UNIT_DIST 10.0
#define QUIT_ALPHA 0.99
#define QUIT_ALPHA_L 0.95

#ifdef PERFORMANCE_MODE
#define RAY_STEP 0.035
#define RAY_STEP_L 0.05
#else
#define RAY_STEP 0.025
#define RAY_STEP_L 0.025
#define SMOOTHING
#endif

#define CAM_THETA (0.2 * iTime)
#define CAM_PHI (HALFPI - 0.2)
#define LIGHT_POS (0.9 * POLAR(CAM_THETA + PI * 0.15) + vec3(0.0, 2.0, 0.0))

// cf. iq https://www.shadertoy.com/view/4sfGzS
float noise(sampler2D randSrc, vec3 x)
{
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    vec2 uv = (i.xy + vec2(37.0, 17.0) * i.z) + f.xy;
    vec2 rg = textureLod(randSrc, (uv + 0.5) / 256.0, 0.0).yx;
    return mix(rg.x, rg.y, f.z);
}

float fbm(sampler2D randSrc, vec3 p)
{
    p *= 0.6;
    float v = noise(randSrc, p);

    p *= 0.3;
    v = mix(v, noise(randSrc, p), 0.7);

    p *= 0.3;
    v = mix(v, noise(randSrc, p), 0.7);

    return v;
}

float fDensity(sampler2D randSrc, vec3 lmn, float t)
{
    t += 32.0;

    // Current position adjusted to [-1,1]^3
    vec3 uvw = (lmn - vec3(63.5)) / 63.5;

    // Value used to offset the main density
    float d2 = fbm(randSrc, vec3(0.6, 0.3, 0.6) * lmn + vec3(0.0, 8.0 * t, 0.0));

    // Main density
    float d1 =
        fbm(randSrc, 0.3 * lmn + vec3(0.0, 4.0 * t, 0.0) + 5.0 * vec3(cos(d2 * TWOPI), 2.0 * d2, sin(d2 * TWOPI)));
    d1 = pow(d1, mix(4.0, 12.0, smoothstep(0.6, 1.0, len2Inf(uvw.xz))));

    // Tweak density curve
    float a = 0.02;
    float b = 0.08;
    return 0.02 + 0.2 * smoothstep(0.0, a, d1) + 0.5 * smoothstep(a, b, d1) + 0.18 * smoothstep(b, 1.0, d1);
}

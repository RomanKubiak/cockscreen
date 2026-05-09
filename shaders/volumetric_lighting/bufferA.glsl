// ==========================
// Store density on "page 1"
// ==========================

vec4 doPage1(vec3 lmn)
{
    return vec4(fDensity(iChannel1, lmn, iTime), 1.0, 1.0, 1.0);
}

vec4 getPage1(vec3 lmn)
{
    bool isInit = iFrame < 5;
    return isInit ? doPage1(lmn) : texture(iChannel0, vcubeFromLMN(1, lmn));
}

// ===========================
// Store lighting on "page 2"
// ===========================

float march(vec3 p, vec3 nv)
{
    float lightAmount = 1.0;

    vec2 tRange;
    float didHitBox;
    boxClip(BOX_MIN, BOX_MAX, p, nv, tRange, didHitBox);
    tRange.s = max(0.0, tRange.s);

    if (didHitBox < 0.5)
    {
        return 0.0;
    }

    float t = tRange.s + min(tRange.t - tRange.s, RAY_STEP_L) * hash13(100.0 * p);
    int i = 0;
    for (; i < 150; i++)
    { // Theoretical max steps: (BOX_MAX-BOX_MIN)*sqrt(3)/RAY_STEP_L
        if (t > tRange.t || lightAmount < 1.0 - QUIT_ALPHA_L)
        {
            break;
        }

        vec3 rayPos = p + t * nv;
        vec3 lmn = lmnFromWorldPos(rayPos);

        float density = getPage1(lmn).s;
        float calpha = clamp(density * MAX_ALPHA_PER_UNIT_DIST * RAY_STEP_L, 0.0, 1.0);

        lightAmount *= 1.0 - calpha;

        t += RAY_STEP_L;
    }

    return lightAmount;
}

vec4 doPage2(vec3 lmn)
{
    float density = getPage1(lmn).s;

    vec3 p = worldPosFromLMN(lmn);
    float lightAmount = march(p, normalize(LIGHT_POS - p));

    return vec4(density, lightAmount, 1.0, 1.0);
}

// ==================
// Write to cube map
// ==================

void mainCubemap(out vec4 fragColor, in vec2 fragCoord, in vec3 rayOri, in vec3 rayDir)
{
    vec3 lmn;
    int pageDst;
    lmnFromVCube(rayDir, pageDst, lmn);

    if (pageDst == 1)
    {
        fragColor = doPage1(lmn);
    }
    else if (pageDst == 2)
    {
        fragColor = doPage2(lmn);
    }
    else
    {
        discard;
    }
}

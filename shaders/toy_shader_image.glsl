#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform sampler2D u_texture;
uniform float u_time;
uniform vec2 u_resolution;

const mat3 uneigenvapor =
    mat3(0.0, 0.70710678, 0.70710678, 0.70710678, 0.0, 0.70710678, 0.57735027, 0.57735027, -0.57735027);

float sigmoid(float value, float advance)
{
    float shifted = clamp(value + advance, 0.0, 1.0);
    return sin(shifted * 1.57);
}

vec3 punch(vec3 vapor)
{
    return vec3(sigmoid(vapor.r, -0.05), sigmoid(vapor.g, -0.05), vapor.b);
}

vec3 offset_and_combine(vec2 uv)
{
    vec2 texel = vec2(7.0) / max(u_resolution, vec2(1.0));
    float modulation = 0.35 + 0.25 * sin(u_time * 0.7);
    vec4 vapor_primary = texture2D(u_texture, uv);
    vec4 vapor_offset = texture2D(u_texture, clamp(uv + texel, 0.0, 1.0));
    return vec3(vapor_primary.r, mix(vapor_primary.g, vapor_offset.g, modulation), vapor_primary.b);
}

vec3 light_static(vec2 frag_coord)
{
    float divisor = max(frag_coord.y, 1.0);
    float noise = mod(frag_coord.x + frag_coord.y * 3.0 + u_time * 60.0 / divisor, 7.0);
    return step(abs(noise), 0.08) * vec3(1.0);
}

vec3 unvaporize(vec3 rgb)
{
    return uneigenvapor * rgb;
}

vec3 scanlines(vec2 frag_coord)
{
    float scan_height =
        clamp(sin(frag_coord.y * 2.5 + u_time * 30.0), -1.0, 0.25) * abs(sin(frag_coord.y / 100.0 + u_time * 0.15));
    return vec3(scan_height);
}

void main()
{
    vec2 uv = v_texcoord;
    vec2 frag_coord = uv * u_resolution;
    vec3 vapor_offset = offset_and_combine(uv);
    vec3 color = unvaporize(punch(vapor_offset)) + light_static(frag_coord) + scanlines(frag_coord);
    gl_FragColor = vec4(color, 1.0);
}
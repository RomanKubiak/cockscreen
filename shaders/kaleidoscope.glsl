#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform float u_time;
uniform vec2 u_viewport_size;
uniform sampler2D u_texture;

uniform vec2  u_mouse;
uniform float u_morph;
uniform float u_sides;
uniform float u_rot;
uniform float u_seed;

const float PI  = 3.14159265;
const float TAU = 6.28318530;

float hash(float n){ return fract(sin(n)*43758.5453); }

float sdSeg(vec2 p, vec2 a, vec2 b){
  vec2 pa=p-a, ba=b-a;
  float h=clamp(dot(pa,ba)/dot(ba,ba),0.0,1.0);
  return length(pa-ba*h);
}

float lineMask(float d, float w){
  return smoothstep(w, w*0.05, d)
       + smoothstep(w*3.5, w*0.5, d)*0.35
       + smoothstep(w*8.0, w*1.0, d)*0.10;
}

void main(){
  vec2 uv = v_texcoord * 2.0 - 1.0;
  uv.x *= u_viewport_size.x / u_viewport_size.y;

  vec2 m = u_mouse;
  m.x *= u_viewport_size.x / u_viewport_size.y;

  float n     = u_sides;
  float morph = u_morph;
  float maxR  = 0.336 * (min(u_viewport_size.x, u_viewport_size.y) / u_viewport_size.y);

  float stickLen = maxR * 0.92;
  vec2 dir   = vec2(cos(u_rot), sin(u_rot));
  vec2 lineA = m + dir * stickLen;
  vec2 lineB = m - dir * stickLen;

  float baseR = min((2.0*stickLen/n) / (2.0*sin(PI/n)), maxR);
  float lw    = 0.004;
  float col   = 0.0;

  float ang0 = u_rot + PI*0.5;
  float a0   = ang0;
  float r0   = min(baseR*(0.72 + 0.56*hash(u_seed+13.1)), maxR);

  for(int i=0; i<12; i++){
    float fi     = float(i);
    float fi1    = fi + 1.0;
    float active = step(fi, n-1.0);

    float ua1 = ang0 + (fi1/n)*TAU;
    float j1  = (hash(u_seed+fi1*3.7)-0.5)*0.55*(TAU/n);
    float a1  = ua1 + j1*morph;
    float r1  = min(baseR*(0.72+0.56*hash(u_seed+fi1*7.3+13.1)), maxR);

    vec2 sP0 = mix(lineA, lineB, fi/n);
    vec2 sP1 = mix(lineA, lineB, fi1/n);
    vec2 pP0 = m + vec2(cos(a0), sin(a0))*min(r0, maxR);
    vec2 pP1 = m + vec2(cos(a1), sin(a1))*r1;

    vec2 p0 = mix(sP0, pP0, morph);
    vec2 p1 = mix(sP1, pP1, morph);

    col = max(col, lineMask(sdSeg(uv, p0, p1), lw)*active);
    a0  = mix(a0, a1, active);
    r0  = mix(r0, r1, active);
  }

  gl_FragColor = vec4(vec3(clamp(col, 0.0, 1.0)), 1.0);
}
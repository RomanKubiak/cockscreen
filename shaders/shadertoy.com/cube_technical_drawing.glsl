vec3 point[14];                              // 8 corners + 6 faces center
vec2 proj[8];                                // screen proj of the 8 corners
int face[24] = int[](4, 2, 3, 4, 2, 5, 5, 3, // 2 faces adjacent to each edge
                     2, 0, 0, 3, 1, 2, 3, 1, 0, 4, 5, 0, 4, 1, 1, 5);

// --- utils from https://www.shadertoy.com/view/llySRh
#define S(d, r) smoothstep(-1.5, 1.5, (r) - (d) * R.y) // antialiased draw
float seg(vec2 p, vec2 a, vec2 b)
{ // --- draw segment with round ends
    p -= a, b -= a;
    float h = clamp(dot(p, b) / dot(b, b), 0., 1.); // proj coord on line
    return length(p - b * h);                       // dist to segment
}
#define rot(a) mat2(cos(a + vec4(0, 11, 33, 0)))

void mainImage(out vec4 O, vec2 u)
{
    vec2 R = iResolution.xy, U = (u - .5 * R) / R.y, M = iMouse.xy;
    vec3 P, A, B, C = vec3(0, 0, -4); // C: camera pos 76
    O -= O;
    float t = length(M) < 20. ? iTime : 6.28 * M.x / R.x;
    int i = 0, a, b, c;
    for (; i < 8; i++)                                         // --- compute 3D corners coords + 2D proj
        P = vec3(i % 2, i / 2 % 2, i / 4 % 2) * 2. - 1.        // cubes corner
            + .1 * cos(iTime + vec3(1 + i, 2 + 2 * i, 3 - i)), // jittering
            P.zx *= rot(t), P.zy *= rot(-.5),
        point[i] = P,                                                      // corner i
            point[8 + i / 4] += P / 4.,                                    // faces corner i belongs to
            point[10 + i % 2] += P / 4.,                                   // ( indeed, calc faces center)
            point[12 + i / 2 % 2] += P / 4., P -= C, proj[i] = P.xy / P.z; // screen proj of corner i

#define N(c) dot(cross(point[8 + face[2 * i + c]] - A, B - A), A - C) // dot(Normal,View)
    for (i = 0; i < 12; i++)
        a = i % 4 * (i < 4 ? 1 : 2) - (i / 4 == 1 ? i % 2 : 0), b = a + (4 >> i / 4), // index of line i ends
            A = point[a], B = point[b],                                               // 3D coords of line i  ends
            // F = ivec3(a/4, 2+a%2 ,4+a/2%2),
            // G = ivec3(b/4, 2+b%2 ,4+b/2%2) - F,
            // F.xy= G.x==0 ? F.yz : G.y==0 ? F.xz :F.xy,            // F[c] replaces face[] (but array shorter)
            c = N(0) < 0. ? 1 : 0, N(1) > 0. ? c++ : c, // visibility of adjacent faces
            O = max(O, S(seg(U, proj[a], proj[b]), c < 1   ? 0.
                                                   : c < 2 ? 4.
                                                           : 2.)); // draw segment
}
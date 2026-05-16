// Psychotherapy Shader
// Small red rectangles grow outward from the center over scene time.
// Each cell flickers independently — quick random red fades — while the
// visible frontier expands outward until it fills all four edges.
#ifdef GL_ES
precision mediump float;
#endif

varying vec2 v_texcoord;
uniform float u_time;
uniform vec2 u_resolution;
uniform sampler2D u_texture;

// ----- parameters -----------------------------------------------------------
const float kSceneDuration = 60.0; // seconds until the grid fully fills
const float kCellsX = 82.0;        // horizontal cell count
const float kCellsY = 50.0;        // vertical cell count
const float kFlickerSpeed = 8.0;   // how fast each cell flickers (Hz-ish)
uniform float u_max_red;            // brightest red any cell reaches (AD7 pot)
// ---------------------------------------------------------------------------

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
}

// Per-cell flicker: cycles through bright and dark at a cell-unique rate.
float flicker(vec2 cell_id, float t)
{
    float rate = kFlickerSpeed * (0.5 + hash(cell_id + vec2(7.3, 2.1)));
    float phase = hash(cell_id + vec2(13.7, 5.9)) * 6.2832;
    float s = 0.5 + 0.5 * sin(t * rate + phase);
    // Add a second harmonic for irregular pulsing.
    float s2 = 0.5 + 0.5 * sin(t * rate * 1.618 + phase * 0.7);
    return mix(s, s2, 0.4);
}

// Map flicker value to a shade of red (dark maroon → bright red).
vec3 red_shade(float f)
{
    // f in [0,1]: 0 → near-black, 1 → saturated red
    float r = f * u_max_red;
    return vec3(r, r * 0.04, r * 0.02);
}

void main()
{
    vec2 uv = v_texcoord;
    // Normalized scene progress [0,1].
    float progress = clamp(u_time / kSceneDuration, 0.0, 1.0);

    // Cell grid: which cell does this fragment belong to?
    vec2 cell_uv = uv * vec2(kCellsX, kCellsY);
    vec2 cell_id = floor(cell_uv); // integer cell index (0..N-1)

    // Normalised cell coordinates [0,1].
    float cx = cell_id.x / (kCellsX - 1.0);
    float cy = cell_id.y / (kCellsY - 1.0);

    // Chebyshev distance from the center: 0 at (0.5,0.5), 1 at all four edges.
    // Cells expand outward from the center so every edge gets the frontier outline.
    float dist = max(abs(cx - 0.5), abs(cy - 0.5)) * 2.0;

    // A cell becomes active when the frontier passes it.
    // Add a small per-cell random offset so cells don't all pop on at once.
    float jitter = hash(cell_id) * 0.04;
    float arrival = dist + jitter;

    // How long since this cell became active, normalised to [0,1].
    // The appearance threshold is lerped from 0 → arrival as progress rises.
    float cell_progress = clamp((progress - arrival) / max(1.0 - arrival, 0.001), 0.0, 1.0);

    // Overall envelope: cells are nearly invisible at first (cell_progress≈0),
    // grow to full brightness.  Ease-in with smoothstep.
    float envelope = smoothstep(0.0, 1.0, cell_progress);

    // Per-cell fast flicker
    float f = flicker(cell_id, u_time);

    // Final red intensity = envelope × flickered shade
    vec3 color = red_shade(f * envelope);

    gl_FragColor = vec4(color, f * envelope);
}

precision mediump float;

varying vec2 v_texcoord;
uniform vec2 u_viewport_size;
uniform float u_time;
uniform float u_audio_level;
uniform float u_midi_primary;
uniform float u_midi_secondary;
uniform float u_midi_notes[8];
uniform float u_midi_velocities[8];
uniform float u_midi_ages[8];
uniform float u_midi_channels[8];
uniform sampler2D u_texture;
uniform sampler2D u_note_label_atlas;
uniform vec2 u_note_label_grid;

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

vec2 hash21(float p)
{
    return vec2(hash11(p), hash11(p + 19.19));
}

float circle_mask(vec2 p, vec2 center, float radius)
{
    return 1.0 - smoothstep(radius * 0.72, radius, length(p - center));
}

float note_size_modulation(float note)
{
    float phase = u_time * (0.7 + fract(note * 0.031) * 1.6) + note * 0.13;
    return 0.75 + 0.25 * sin(phase);
}

float note_label_mask(float note_number, vec2 local_uv)
{
    if (local_uv.x < 0.0 || local_uv.x > 1.0 || local_uv.y < 0.0 || local_uv.y > 1.0)
    {
        return 0.0;
    }

    float note_index = clamp(floor(note_number + 0.5), 0.0, 127.0);
    float column = mod(note_index, u_note_label_grid.x);
    float row = floor(note_index / u_note_label_grid.x);
    vec2 atlas_uv = (vec2(column, row) + local_uv) / u_note_label_grid;
    return texture2D(u_note_label_atlas, atlas_uv).a;
}

float note_label_edge_mask(float note_number, vec2 local_uv)
{
    if (local_uv.x < 0.0 || local_uv.x > 1.0 || local_uv.y < 0.0 || local_uv.y > 1.0)
    {
        return 0.0;
    }

    float note_index = clamp(floor(note_number + 0.5), 0.0, 127.0);
    float column = mod(note_index, u_note_label_grid.x);
    float row = floor(note_index / u_note_label_grid.x);
    vec2 atlas_uv = (vec2(column, row) + local_uv) / u_note_label_grid;
    return texture2D(u_note_label_atlas, atlas_uv).a;
}

void main()
{
    vec4 base = texture2D(u_texture, v_texcoord);
    vec2 aspect = vec2(u_viewport_size.x / max(u_viewport_size.y, 1.0), 1.0);
    vec2 p = (v_texcoord * 2.0 - 1.0) * aspect;

    vec3 dots = vec3(0.0);
    float alpha = 0.0;

    for (int i = 0; i < 8; ++i)
    {
        float age = u_midi_ages[i];
        if (age < 0.0)
        {
            continue;
        }

        float note = u_midi_notes[i];
        float velocity = clamp(u_midi_velocities[i], 0.0, 1.0);
        float channel = u_midi_channels[i];
        float life = clamp(1.0 - age / 1.05, 0.0, 1.0);
        float seed = note * 0.173 + channel * 11.37 + float(i) * 37.11;
        vec2 center = hash21(seed) * 1.6 - 0.8;
        float base_radius = mix(0.035, 0.14, velocity);
        float animated_radius = base_radius * note_size_modulation(note) * (0.65 + 0.35 * life);
        animated_radius *= 1.25;
        float dot_shape = circle_mask(p, center, animated_radius);
        vec2 label_uv = ((p - center) / max(animated_radius * 0.72, 0.0001)) * 0.5 + 0.5;
        float label_mask = note_label_mask(note, label_uv);
        float label_edge = note_label_edge_mask(note, label_uv * 1.05 - 0.025);
        vec3 dot_color = vec3(mix(0.35, 1.0, velocity), 0.0, 0.0);
        vec3 label_color = vec3(1.0, 1.0, 1.0);
        vec3 label_shadow = vec3(0.0, 0.0, 0.0);
        dots += dot_color * dot_shape;
        dots += label_shadow * label_edge * dot_shape * 0.9;
        dots += label_color * label_mask * dot_shape;
        alpha = max(alpha, step(0.001, dot_shape));
    }

    vec3 color = clamp(base.rgb + dots, 0.0, 1.0);

    gl_FragColor = vec4(color, max(base.a, alpha));
}
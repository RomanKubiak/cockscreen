precision mediump float;

varying vec2 v_texcoord;
uniform vec2 u_viewport_size;
uniform float u_time;
uniform sampler2D u_texture;

void main()
{
    float loop = 0.5 + 0.5 * sin(u_time * 0.9);
    float block_size = mix(48.0, 1.0, loop);
    vec2 pixel_space = v_texcoord * u_viewport_size;
    vec2 block_center = floor(pixel_space / block_size) * block_size + block_size * 0.5;
    vec2 sample_uv = block_center / max(u_viewport_size, vec2(1.0));

    vec4 color = texture2D(u_texture, sample_uv);
    float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    vec3 reduced = mix(color.rgb, vec3(luma), 0.08);

    gl_FragColor = vec4(clamp(reduced, 0.0, 1.0), color.a);
}

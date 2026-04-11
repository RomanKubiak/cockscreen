precision mediump float;

varying vec2 v_texcoord;
uniform vec2 u_viewport_size;
uniform float u_pixelize_amount;
uniform sampler2D u_texture;

void main()
{
    float amount = clamp(u_pixelize_amount, 0.0, 1.0);
    float block_size = mix(1.0, 56.0, amount);

    vec2 pixel_space = v_texcoord * u_viewport_size;
    vec2 block_origin = floor(pixel_space / block_size) * block_size;
    vec2 block_center = block_origin + block_size * 0.5;
    vec2 sample_uv = block_center / max(u_viewport_size, vec2(1.0));

    vec4 color = texture2D(u_texture, sample_uv);
    gl_FragColor = vec4(color.rgb, color.a);
}
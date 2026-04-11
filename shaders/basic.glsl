precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;

void main()
{
    vec4 color = texture2D(u_texture, v_texcoord);
    float luma = dot(color.rgb, vec3(0.299, 0.587, 0.114));
    vec3 graded = mix(color.rgb, vec3(luma), 0.12);
    graded = (graded - 0.5) * 1.08 + 0.5;

    vec2 offset = v_texcoord - vec2(0.5);
    float vignette = smoothstep(0.95, 0.30, dot(offset, offset) * 1.85);
    graded *= vignette;

    gl_FragColor = vec4(clamp(graded, 0.0, 1.0), color.a);
}

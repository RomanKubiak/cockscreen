precision mediump float;

varying vec2 v_texcoord;
uniform sampler2D u_texture;

void main()
{
    vec4 color = texture2D(u_texture, v_texcoord);
    vec3 boosted = color.rgb * 1.85;
    boosted = pow(clamp(boosted, 0.0, 1.0), vec3(0.85));
    boosted = mix(boosted, vec3(dot(boosted, vec3(0.299, 0.587, 0.114))), 0.04);

    gl_FragColor = vec4(clamp(boosted, 0.0, 1.0), color.a);
}

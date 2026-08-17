#version 450
layout(set = 0, binding = 0) uniform samplerCubeArray probeTexture;
layout(set = 0, binding = 1) uniform sampler2D dynamicTexture;
layout(set = 0, binding = 2) uniform sampler2D mediaTexture;
layout(location = 0) out vec4 outColor;
void main()
{
    int band = min(int(gl_FragCoord.x / 8.0), 7);
    vec2 localUv = fract(gl_FragCoord.xy / 8.0);
    if (band < 6)
    {
        const vec3 directions[6] = vec3[6](
            vec3(1.0, 0.0, 0.0), vec3(-1.0, 0.0, 0.0),
            vec3(0.0, 1.0, 0.0), vec3(0.0, -1.0, 0.0),
            vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, -1.0));
        float lod = gl_FragCoord.y < 32.0 ? 0.0 : 1.0;
        outColor = textureLod(probeTexture, vec4(directions[band], 0.0), lod);
    }
    else if (band == 6)
        outColor = texture(dynamicTexture, localUv);
    else
        outColor = texture(mediaTexture, localUv);
}

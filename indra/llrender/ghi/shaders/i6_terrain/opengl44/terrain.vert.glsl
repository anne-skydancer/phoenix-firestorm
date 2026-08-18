#version 440 core
layout(std140) uniform TerrainData { mat4 viewProjection; mat4 modelTransform; mat4 normalTransform; vec4 uvOffsetScale[4]; vec4 uvRotation[4]; vec4 baseColorFactors[4]; vec4 emissiveMetallic[4]; vec4 roughnessAlpha[4]; vec4 terrainParams; };
layout(location=0) in vec3 inPosition; layout(location=1) in vec3 inNormal; layout(location=3) in vec2 inCompositionCoord;
layout(location=0) out vec3 localPosition; layout(location=1) out vec3 worldNormal; layout(location=2) out vec2 compositionCoord;
void main(){ gl_Position=viewProjection*modelTransform*vec4(inPosition,1); localPosition=inPosition; worldNormal=normalize(mat3(normalTransform)*inNormal); compositionCoord=inCompositionCoord; }

#version 440 core

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inVertexColor;
layout(location = 2) in vec2 inInstanceOffset;
layout(location = 3) in vec4 inInstanceColor;

layout(location = 0) out vec4 vertexColor;

void main()
{
    gl_Position = vec4(inPosition.xy + inInstanceOffset, inPosition.z, 1.0);
    vertexColor = inVertexColor * inInstanceColor;
}

#version 460 core

layout(std140, binding = 0) uniform FrameData
{
    mat4 transform;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 vertexColor;

void main()
{
    gl_Position = transform * vec4(inPosition, 1.0);
    vertexColor = inColor;
}

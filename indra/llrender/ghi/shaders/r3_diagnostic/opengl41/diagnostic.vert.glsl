#version 410 core

layout(std140) uniform FrameData
{
    mat4 transform;
};

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec2 texCoord;

void main()
{
    gl_Position = transform * vec4(inPosition, 1.0);
    texCoord = inTexCoord;
}

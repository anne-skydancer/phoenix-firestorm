#version 410 core

layout(location = 0) in vec4 vertexColor;
layout(location = 0) out vec4 outColor;

void main()
{
    outColor = vertexColor;
}

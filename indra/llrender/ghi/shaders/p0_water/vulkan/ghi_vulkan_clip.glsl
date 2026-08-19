#ifndef GHI_VULKAN_CLIP_GLSL
#define GHI_VULKAN_CLIP_GLSL
vec4 ghiVulkanClip(vec4 clip)
{
    clip.y = -clip.y;
    clip.z = (clip.z + clip.w) * 0.5;
    return clip;
}
#endif

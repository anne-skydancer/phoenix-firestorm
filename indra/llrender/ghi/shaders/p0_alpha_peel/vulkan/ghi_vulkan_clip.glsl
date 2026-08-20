vec4 ghiToVulkanClip(vec4 clip)
{
    clip.y = -clip.y;
    clip.z = 0.5 * (clip.z + clip.w);
    return clip;
}

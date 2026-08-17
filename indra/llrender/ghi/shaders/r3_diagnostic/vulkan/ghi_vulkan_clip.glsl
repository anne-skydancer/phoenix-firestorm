vec4 ghiToVulkanClip(vec4 canonicalClip)
{
    // The incremental renderer keeps the viewer's OpenGL clip convention as
    // canonical. Vulkan uses a positive-height viewport, so Y and the
    // [-W,+W] depth interval are converted here and nowhere else.
    canonicalClip.y = -canonicalClip.y;
    canonicalClip.z = 0.5 * (canonicalClip.z + canonicalClip.w);
    return canonicalClip;
}

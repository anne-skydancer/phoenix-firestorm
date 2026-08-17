vec4 ghiToVulkanClip(vec4 canonicalClip)
{
    canonicalClip.y = -canonicalClip.y;
    canonicalClip.z = 0.5 * (canonicalClip.z + canonicalClip.w);
    return canonicalClip;
}

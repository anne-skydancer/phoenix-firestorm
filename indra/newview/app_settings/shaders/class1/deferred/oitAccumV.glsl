/**
 * @file oitAccumV.glsl
 * WBOIT (weighted-blended order-independent transparency) accumulation pass -
 * vertex shader. Transforms the geometry and forwards view-space depth for the
 * fragment weight. <FS>
 *
 * $LicenseInfo:firstyear=2024&license=viewerlgpl$ ... $/LicenseInfo$
 */

uniform mat4 modelview_projection_matrix;
uniform mat4 modelview_matrix;

in vec3 position;

out float vary_view_z; // view-space z (negative in front of the camera)

void main()
{
    vec4 vpos = modelview_matrix * vec4(position.xyz, 1.0);
    vary_view_z = vpos.z;
    gl_Position = modelview_projection_matrix * vec4(position.xyz, 1.0);
}

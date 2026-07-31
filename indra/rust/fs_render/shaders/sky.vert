#version 450
// Faithful port of class1/deferred/skyV.glsl (the EEP haze dome -- NO texture; the
// sky's color IS this math). Uniforms arrive via the sky/water UBO slot layout:
// [mvp | 12 vec4s of aux], packed by the tap from the bound shader's mValue cache.
layout(location = 0) in vec4 pos;
layout(location = 1) in vec2 uv; // dome TC0, unused
layout(set = 0, binding = 0) uniform U {
    mat4 mvp;
    vec4 a0;  // camPosLocal.xyz, max_y
    vec4 a1;  // lightnorm.xyz, sun_up_factor
    vec4 a2;  // sunlight_color.xyz, density_multiplier
    vec4 a3;  // moonlight_color.xyz, sun_moon_glow_factor
    vec4 a4;  // ambient_color.xyz, haze_density
    vec4 a5;  // blue_horizon.xyz, haze_horizon
    vec4 a6;  // blue_density.xyz, cloud_shadow
    vec4 a7;  // glow.xyz, blend_factor
    vec4 a8;
    vec4 a9;
    vec4 a10;
    vec4 a11;
} u;
layout(location = 0) out vec3 v_haze;

void main() {
    gl_Position = u.mvp * vec4(pos.xyz, 1.0);

    vec3 camPosLocal = u.a0.xyz;
    float max_y = u.a0.w;
    vec3 lightnorm = u.a1.xyz;
    float sun_up_factor = u.a1.w;
    vec3 sunlight_color = u.a2.xyz;
    float density_multiplier = u.a2.w;
    vec3 moonlight_color = u.a3.xyz;
    float sun_moon_glow_factor = u.a3.w;
    vec3 ambient_color = u.a4.xyz;
    float haze_density_f = u.a4.w;
    vec3 blue_horizon = u.a5.xyz;
    float haze_horizon = u.a5.w;
    vec3 blue_density = u.a6.xyz;
    float cloud_shadow = u.a6.w;
    vec3 glow = u.a7.xyz;

    vec3 rel_pos = pos.xyz - camPosLocal + vec3(0.0, 50.0, 0.0);
    if (rel_pos.y > 0.0) { rel_pos *= (max_y / rel_pos.y); }
    if (rel_pos.y < 0.0) { rel_pos *= (-32000.0 / rel_pos.y); }

    vec3 rel_pos_norm = normalize(rel_pos);
    float rel_pos_len = length(rel_pos);
    float rel_dot = dot(rel_pos_norm, lightnorm);

    vec3 sunlight = (sun_up_factor == 1.0) ? sunlight_color : moonlight_color * 0.7;

    vec3 light_atten = (blue_density + vec3(haze_density_f * 0.25)) * (density_multiplier * max_y);
    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density_f)), vec3(1e-6));
    vec3 blue_weight = blue_density / combined_haze;
    vec3 haze_weight = vec3(haze_density_f) / combined_haze;

    float off_axis = 1.0 / max(1e-6, max(0.0, rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);

    float density_dist = rel_pos_len * density_multiplier;
    combined_haze = exp(-combined_haze * density_dist);

    float haze_glow = max(1.0 - rel_dot, 0.001);
    haze_glow *= glow.x;
    haze_glow = pow(haze_glow, glow.z);
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));

    vec3 color = (blue_horizon * blue_weight * (sunlight + ambient_color)
               + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color));
    color *= (1.0 - combined_haze);

    vec3 ambient = ambient_color + max(vec3(0.0), (1.0 - ambient_color)) * cloud_shadow * 0.5;
    sunlight *= max(0.0, (1.0 - cloud_shadow));
    vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight + ambient)
                         + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient));
    combined_haze = sqrt(combined_haze);
    color += (add_below_cloud - color) * (1.0 - sqrt(combined_haze));

    v_haze = color;
}

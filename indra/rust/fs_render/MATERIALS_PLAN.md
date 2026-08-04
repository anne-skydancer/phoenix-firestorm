# Materials stratum plan (fs_render)

Order (user, load-bearing): **legacy texturing → materials → PBR → HDR**. Materials sits ON legacy
texturing (diffuse-only N·L, done in resolve.frag) and UNDER PBR. HDR is LAST. Each step is verified
apples-to-apples against the REAL OGL shader via the fs_ogl_ref oracle bench — no screenshots, no
patching. OGL is invariant; fix fs_render to match.

## What OGL does (traced 2026-08-04, the invariant)

Legacy Blinn-Phong materials (the 2014 materials system): diffuse + normal map + specular map
(color + exponent/glossiness) + environment intensity + emissive + alpha modes.

**G-buffer** (`pipeline.cpp:381` addDeferredAttachments; `materialF.glsl` deferred main):
- RT0 diffuse.rgb (sRGB) + emissive in .a
- RT1 = GL_RGBA8 — legacy: (specular_color.rgb, specular_exponent/glossiness); PBR: ORM (occ,rough,metal)
- RT2 = GL_RGBA16 — encodeNormal: normal.xy, env_intensity.z, gbuffer_flag.w
- RT3 = GL_RGB16F (optional, RenderEnableEmissiveBuffer) — emissive color

**Resolve** (softenLightF legacy `else` branch):
- baseColor = srgb_to_linear(diffuse); spec.rgb = srgb_to_linear(spec.rgb); spec.a = glossiness
- ambient: sampleReflectionProbesLegacy (probe-dependent; classic uses amblit directly)
- direct Blinn-Phong specular (PROBE-INDEPENDENT): if glossiness>0 and nl,nh>0:
    lit = min(nl*6,1); fres = pow(1-vh,5)*0.4+0.5; gt = max(0,min(2nh·nv/vh, 2nh·nl/vh));
    scol = shadow·fres·lightFunc(nh,gloss)·gt/(nh·nl);  color += lit·scol·sunlit·spec.rgb
    lightFunc = CLOSED FORM (pipeline.cpp:1624): n = gloss²·368(RenderSpecularExponent);
      lightFunc(nh,gloss) = pow(nh,n) · ((n+2)(n+4))/(8·π·(2^(-n/2)+n))
- emissive: color = mix(color, diffuse, emissive)
- env reflections applyGlossEnv/applyLegacyEnv (PROBE-dependent) → deferred to the probe stratum
- needs the VIEW VECTOR v = -normalize(eye_pos) (world: normalize(cam - world_pos)) from depth+inv_proj

## fs_render today (mapped 2026-08-04)

2 RTs only: RT0 Rgba8Unorm (albedo + flag.a), RT1 Rgba16Float (world normal.xyz + unused env.w).
No spec/ORM/emissive RT. Depth NOT bound to resolve. resolve hardcodes metallic=0/ao=1/f0=0.04, no
specular/gloss/env/emissive. Fill: live_gb (HAS_ATMOS, EYE-space normal — latent bug vs resolve's
world assumption), terrain_gb (HAS_ATMOS, world normal), pbrterrain_gb (HAS_PBR, linear base, world
normal). DrawDesc has dead material placeholders (material_model, pbr[metal,rough,emis,env],
emissive_color, orm_tex, emissive_tex). Typed scene carries no per-draw material. No material-bearing
prim/mesh geometry in the typed path yet (that lands with the meshes stratum).

fs_render deliberately keeps WORLD normal in RT1.xyz (3 ch) → flag lives in RT0.a, env in RT1.w.

## Build order (each verified against the oracle bench, each committable)

- **M0 — view vector (shared prerequisite for ALL specular).** Bind the depth G-buffer to the resolve
  pass; reconstruct world position from depth + inv_view_proj (already in the sky UBO u0-15) + cam
  (u16-18); v = normalize(cam - world_pos). Use it for the REAL Fresnel (1-F) in the PBR branch.
  Verify: feed the resolve bench the SAME inv_proj (as inv_view_proj) + cam=origin as the oracle
  fixture → the atmospherics PBR center ratio tightens (closes the view-vector part of that residual).

- **M1 — G-buffer expansion.** Add RT2 = Rgba8Unorm (spec color+exponent legacy / ORM PBR), start using
  RT1.w = env_intensity, add RT3 = Rgba16Float (emissive, gated like RenderEnableEmissiveBuffer). Update
  all three fill shaders + the resolve bind group + pipelines. Fill writes neutral until material data
  is plumbed; the bench feeds synthetic material G-buffers.

- **M2 — legacy-material resolve (materials proper).** resolve.frag legacy branch: read spec (RT2) +
  env (RT1.w) + emissive (RT3); do the direct Blinn-Phong specular (closed-form lightFunc) + emissive
  mix; classic/non-classic split intact. Env reflections = probe stratum (note the boundary). Verify:
  extend the bench fixture to a specular+emissive HAS_ATMOS material → resolve legacy branch vs
  softenLightF legacy branch, both regimes.

- **M3 — fill capability + bridge.** The fill shaders write spec/env/emissive from material data; wire
  the DrawDesc/typed-scene material fields (currently dead). Live effect lands when prim/mesh geometry
  renders (meshes stratum), but the capability + bench coverage exist now.

## Then PBR stratum (separate plan): real ORM (roughness/metallic) in RT2 → pbrPunctual GGX specular +
## real (1-F) + probe IBL. Closes the atmospherics PBR residuals (specPunc, probe-ambient). Then HDR.

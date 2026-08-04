//! OGL shader reproduction: assemble the REAL viewer `.glsl` the viewer's way, transform loose
//! uniforms/samplers into the Vulkan shape (std140 UBO + separate texture/sampler), and compile to
//! SPIR-V. Lifted verbatim from `vkharness` (H3b-3 / H3b-3c, proven) so `fs_ogl_ref` runs the exact
//! OGL shaders headless. Kept a copy (not a vkharness dep) so the proven harness can't be broken;
//! consolidate into a shared crate later.

use shaderc;

pub const SHADER_ROOT: &str = "c:/fs/firestorm-upstream/indra/newview/app_settings/shaders";

/// Fragment attach order for softenLight (from attachShaderFeatures + its flags), softenLightF last.
/// Each is "subdir/name.glsl", class-resolved (class3->2->1) at read time.
pub const SOFTENLIGHT_FRAG_PIECES: &[&str] = &[
    "deferred/globalF.glsl",
    "environment/srgbF.glsl",
    "windlight/atmosphericsVarsF.glsl",
    "windlight/atmosphericsHelpersF.glsl",
    "deferred/deferredUtil.glsl",
    "deferred/gbufferUtil.glsl",
    "deferred/screenSpaceReflUtil.glsl",
    "deferred/shadowUtil.glsl",
    "deferred/reflectionProbeF.glsl",
    "windlight/gammaF.glsl",
    "windlight/atmosphericsFuncs.glsl",
    "windlight/atmosphericsF.glsl",
    "environment/waterFogF.glsl",
    "deferred/softenLightF.glsl",
];

/// GLSL -> SPIR-V (Vulkan target).
pub fn compile_glsl(source: &str, kind: shaderc::ShaderKind, name: &str) -> Vec<u32> {
    let compiler = shaderc::Compiler::new().expect("shaderc compiler init");
    let mut opts = shaderc::CompileOptions::new().expect("shaderc options");
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_2 as u32);
    opts.set_source_language(shaderc::SourceLanguage::GLSL);
    let artifact = compiler
        .compile_into_spirv(source, kind, name, "main", Some(&opts))
        .unwrap_or_else(|e| panic!("GLSL->SPIR-V failed for {name}: {e}"));
    if artifact.get_num_warnings() > 0 {
        log::warn!("{name}: shaderc warnings: {}", artifact.get_warning_messages());
    }
    artifact.as_binary().to_vec()
}

/// Extract the declared name from a single-line `uniform ... ;` decl (strips array suffix + quals).
fn decl_name_uniform(line: &str) -> Option<String> {
    let head = line.split(';').next()?;
    let head = head.split('[').next()?;
    let name = head.split_whitespace().last()?;
    if name == "uniform" || !name.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return None;
    }
    Some(name.to_string())
}

/// Extract the name from a global `const TYPE name = ...;` decl.
fn decl_name_const(line: &str) -> Option<String> {
    let head = line.split('=').next()?;
    let name = head.split_whitespace().last()?;
    if !name.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return None;
    }
    Some(name.to_string())
}

/// Keep-first dedup of GLOBAL (brace-depth-0) `uniform`/`const` declarations (= the viewer's
/// multi-object linker merge, which flattening to one TU for SPIR-V surfaces as redefinitions).
pub fn dedup_global_decls(src: &str) -> (String, usize) {
    use std::collections::HashSet;
    let mut seen: HashSet<String> = HashSet::new();
    let mut depth: i32 = 0;
    let mut dropped = 0usize;
    let mut out = String::with_capacity(src.len());
    for line in src.lines() {
        let trimmed = line.trim_start();
        let mut drop_line = false;
        if depth == 0 {
            let name = if trimmed.starts_with("uniform ") {
                decl_name_uniform(trimmed)
            } else if trimmed.starts_with("const ") {
                decl_name_const(trimmed)
            } else {
                None
            };
            if let Some(n) = name {
                if !seen.insert(n) {
                    drop_line = true;
                }
            }
        }
        if drop_line {
            out.push_str("// [dedup] ");
            dropped += 1;
        }
        out.push_str(line);
        out.push('\n');
        for c in line.chars() {
            if c == '{' {
                depth += 1;
            } else if c == '}' {
                depth -= 1;
            }
        }
    }
    (out, dropped)
}

/// Assemble the softenLight fragment source the viewer's way (header + class-resolved pieces + dedup).
pub fn assemble_softenlight_fragment() -> String {
    let mut s = String::new();
    s.push_str("#version 450\n");
    s.push_str("precision highp float;\n");
    s.push_str("#define FRAGMENT_SHADER 1\n");
    s.push_str("#define GBUFFER_FLAG_SKIP_ATMOS 0.0\n");
    s.push_str("#define GBUFFER_FLAG_HAS_ATMOS 0.34\n");
    s.push_str("#define GBUFFER_FLAG_HAS_PBR 0.67\n");
    s.push_str("#define GBUFFER_FLAG_HAS_HDRI 1.0\n");
    s.push_str("#define GET_GBUFFER_FLAG(data, flag) (abs(data-flag)< 0.1)\n");
    s.push_str("struct GBufferInfo { vec4 albedo; vec4 specular; vec3 normal; vec4 emissive; float gbufferFlag; float envIntensity; };\n");
    s.push_str("#define HAS_SUN_SHADOW 1\n");
    s.push_str("#define HAS_SSAO 1\n");
    s.push_str("#define HAS_EMISSIVE 1\n"); // else getGBuffer never reads emissiveRect -> PBR emissive dropped
    s.push_str("#define REF_SAMPLE_COUNT 32\n");
    s.push_str("#define IS_AMD_CARD 1\n");
    for piece in SOFTENLIGHT_FRAG_PIECES {
        let (sub, name) = piece.split_once('/').expect("piece has a subdir");
        let mut found = None;
        for cls in ["class3", "class2", "class1"] {
            let p = format!("{SHADER_ROOT}/{cls}/{sub}/{name}");
            if std::path::Path::new(&p).exists() {
                found = Some((cls, p));
                break;
            }
        }
        match found {
            Some((cls, p)) => {
                let body = std::fs::read_to_string(&p).unwrap_or_default();
                s.push_str(&format!("\n// ===== {piece}  [{cls}] =====\n"));
                s.push_str(&body);
                s.push('\n');
            }
            None => {
                log::warn!("could NOT resolve include {piece}");
                s.push_str(&format!("\n// ===== {piece}  [NOT FOUND] =====\n"));
            }
        }
    }
    let (deduped, dropped) = dedup_global_decls(&s);
    log::info!("softenLight: dedup merged {dropped} duplicate global decls (= linker merge)");
    deduped
}

fn is_sampler_decl(trimmed: &str) -> bool {
    match trimmed.split_whitespace().nth(1) {
        Some(ty) => {
            ty.starts_with("sampler") || ty.starts_with("isampler") || ty.starts_with("usampler") || ty.starts_with("image")
        }
        None => false,
    }
}

/// Map a GLSL combined-sampler type to its Vulkan SEPARATE (texture, sampler) pair.
fn split_sampler_type(st: &str) -> (String, String) {
    let is_shadow = st.contains("Shadow");
    let smp_type = if is_shadow { "samplerShadow" } else { "sampler" };
    let suffix = st.strip_prefix("sampler").unwrap_or(st).replace("Shadow", "");
    (format!("texture{suffix}"), smp_type.to_string())
}

/// Transform loose uniforms -> anonymous std140 UBO (binding 0) + samplers -> separate
/// texture/sampler (bindings 1+). Returns (transformed_src, n_ubo_members, n_samplers).
pub fn ubo_transform(src: &str) -> (String, usize, usize) {
    let mut members: Vec<String> = Vec::new();
    let mut binding: u32 = 1; // binding 0 reserved for the UBO block
    let mut samplers = 0usize;
    let mut depth: i32 = 0;
    let mut body: Vec<String> = Vec::new();

    for line in src.lines() {
        let trimmed = line.trim_start();
        let mut out_line = line.to_string();
        if depth == 0 && trimmed.starts_with("uniform ") {
            if is_sampler_decl(trimmed) {
                let st = trimmed.split_whitespace().nth(1).unwrap_or("sampler2D");
                let name = decl_name_uniform(trimmed).unwrap_or_default();
                let (tex_type, smp_type) = split_sampler_type(st);
                let b_tex = binding;
                let b_smp = binding + 1;
                binding += 2;
                out_line = format!(
                    "layout(set = 0, binding = {b_tex}) uniform {tex_type} {name}_tex;\n\
                     layout(set = 0, binding = {b_smp}) uniform {smp_type} {name}_smp;\n\
                     #define {name} {st}({name}_tex, {name}_smp)"
                );
                samplers += 1;
            } else if trimmed.contains('=') {
                out_line = line.replacen("uniform", "const", 1);
            } else if trimmed.contains(';') {
                let member = trimmed.strip_prefix("uniform ").unwrap().trim();
                members.push(member.to_string());
                out_line = format!("// [ubo] {trimmed}");
            } else {
                out_line = format!("layout(set = 0, binding = {binding}) {trimmed}");
                binding += 1;
            }
        } else if depth == 0
            && trimmed.starts_with("layout")
            && trimmed.contains("uniform")
            && !trimmed.contains(';')
        {
            out_line = format!("layout(set = 0, binding = {binding}) {trimmed}");
            binding += 1;
        }
        body.push(out_line);
        for c in line.chars() {
            if c == '{' {
                depth += 1;
            } else if c == '}' {
                depth -= 1;
            }
        }
    }

    let mut block = String::new();
    block.push_str("layout(std140, set = 0, binding = 0) uniform SoftenFrameBlock {\n");
    for m in &members {
        block.push_str("    ");
        block.push_str(m);
        block.push('\n');
    }
    block.push_str("};\n");

    let mut out = String::with_capacity(src.len() + block.len());
    let mut injected = false;
    for l in body {
        if !injected && l.starts_with("// ===== ") {
            out.push_str("\n// ---- value-uniforms -> anonymous std140 UBO ----\n");
            out.push_str(&block);
            out.push('\n');
            injected = true;
        }
        out.push_str(&l);
        out.push('\n');
    }
    if !injected {
        out.push_str(&block);
    }
    (out, members.len(), samplers)
}

/// Assemble + UBO-transform softenLight, compile to Vulkan SPIR-V words. Writes the transformed
/// source + the SPIR-V to c:/fs/ for inspection. Returns (spirv_words, n_ubo_members, n_samplers).
pub fn build_softenlight_ubo_spirv() -> Result<(Vec<u32>, usize, usize), String> {
    let deduped = assemble_softenlight_fragment();
    let (src, n_members, n_samplers) = ubo_transform(&deduped);
    let out = "c:/fs/fsref_softenLight_ubo.frag";
    let _ = std::fs::write(out, &src);
    log::info!(
        "fs_ogl_ref softenLight UBO transform -> {}  ({} UBO members, {} samplers, {} lines)",
        out, n_members, n_samplers, src.lines().count()
    );
    let compiler = shaderc::Compiler::new().expect("shaderc");
    let mut opts = shaderc::CompileOptions::new().expect("shaderc opts");
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_3 as u32);
    opts.set_auto_map_locations(true);
    match compiler.compile_into_spirv(&src, shaderc::ShaderKind::Fragment, "softenLightF.ubo", "main", Some(&opts)) {
        Ok(art) => {
            let spv = art.as_binary().to_vec();
            let bytes: &[u8] = bytemuck::cast_slice(&spv);
            let _ = std::fs::write("c:/fs/fsref_softenLight_ubo.spv", bytes);
            Ok((spv, n_members, n_samplers))
        }
        Err(e) => Err(e.to_string()),
    }
}

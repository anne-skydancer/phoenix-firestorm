//! Phase 1 — offline shader sweep (rhi/PLAN.md).
//!
//! Replicates the viewer's shader ASSEMBLY offline (llshadermgr.cpp `loadShaderFile` preamble +
//! `attachShaderFeatures` attach order + class-resolution walk + the multi-object linker's
//! implicit decl merge), flattens each program's stage into ONE translation unit, applies the
//! H3-proven UBO + separate-sampler transforms, and compiles for Vulkan (shaderc, set/binding
//! kept) + validates with spirv-val. Gate: every program in the table compiles + validates.
//!
//! Faithfulness notes (scout 2026-07-31, anchors in llshadermgr.cpp / llviewershadermgr.cpp):
//! - D1: globals (loadBasicShaders `attribs`) and per-program permutations are DISJOINT define
//!   namespaces that never met in one TU in the viewer -- the flattened TU emits their union.
//! - D2: duplicate global uniform/const decls across attached objects are merged by the GL
//!   linker; flattening surfaces them as redefinitions -> keep-first dedup (main.rs).
//! - D3: diffuseLookup emits the SWITCH form (non-NVIDIA); IS_AMD_CARD=1 (our target).
//! - D4: `[EXTRA_CODE_HERE]` marker lines are DROPPED; we hoist all #extension directives to
//!   the TU top instead of replicating the per-object splice (superset semantics, recorded).
//! - D7: defines emit in lexicographic order (std::map).
//! - D13: feature objects compile at their OWN levels (loadBasicShaders table below), not the
//!   program's level; program main files resolve at the program's level.
//! - Canonical settings (recorded): RenderShadowDetail=2 (SUN_SHADOW+SPOT_SHADOW), SSR=off,
//!   probes on level 3 (REFMAP_LEVEL=3, REF_SAMPLE_COUNT=32), mirrors off, emissive off,
//!   SSAO on (HAS_SSAO perms where the table says), sIndexedTextureChannels=4, version 450,
//!   AMD/non-NVIDIA, MAX_JOINTS=110, terrain defaults, sum_lights_class=3.

use crate::SHADER_ROOT;

// ---- A1 config matrix (PHASE2_PLAN.md) --
pub struct SweepConfig {
    pub name: &'static str,
    pub remove: &'static [&'static str],
    pub add: &'static [(&'static str, &'static str)],
    pub nvidia: bool,
    pub probes: bool,
    pub ssr: bool,
}
pub const CONFIGS: &[SweepConfig] = &[
    SweepConfig { name: "canonical",   remove: &[], add: &[], nvidia: false, probes: true, ssr: false },
    SweepConfig { name: "ssr-on",      remove: &[], add: &[("SSR", "1")], nvidia: false, probes: true, ssr: true },
    SweepConfig { name: "probes-off",  remove: &["REFMAP_LEVEL", "REF_SAMPLE_COUNT"], add: &[], nvidia: false, probes: false, ssr: false },
    SweepConfig { name: "emissive-on", remove: &[], add: &[("HAS_EMISSIVE", "1")], nvidia: false, probes: true, ssr: false },
    SweepConfig { name: "shadows-off", remove: &["SUN_SHADOW", "SPOT_SHADOW"], add: &[], nvidia: false, probes: true, ssr: false },
    SweepConfig { name: "nvidia",      remove: &[], add: &[], nvidia: true, probes: true, ssr: false },
];
static CFG_IDX: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
fn cfg() -> &'static SweepConfig { &CONFIGS[CFG_IDX.load(std::sync::atomic::Ordering::Relaxed)] }


/// Name of a single-line global declaration (`uniform T name;`, `in vec4 name;`, `const T n = ..`).
fn line_decl_name(head: &str) -> Option<String> {
    // last token BEFORE stripping arrays -- `out vec4[2] vary_coords;` must yield
    // "vary_coords", not "vec4" (array-type-first syntax); `foo[4];` still yields "foo".
    let head = head.split(';').next()?.split('=').next()?;
    let name = head.split_whitespace().last()?.split('[').next()?;
    if name.chars().all(|c| c.is_alphanumeric() || c == '_') && !name.is_empty() {
        Some(name.to_string())
    } else {
        None
    }
}

/// Flatten-dedup for a single TU: keep-first for depth-0 `uniform`/`const`/`in`/`out` decls AND
/// depth-0 `struct Name { ... };` blocks. All were per-object in GL (merged or private at link);
/// one TU sees them as redefinitions. (Extends the H3b3 dedup, which covered uniform/const only.)
/// Net brace delta of a line, ignoring `//` comments (the [dedup] prefix and GLSL comments).
fn brace_delta(line: &str) -> i32 {
    let code = line.split("//").next().unwrap_or("");
    let mut d = 0i32;
    for c in code.chars() {
        if c == '{' {
            d += 1;
        } else if c == '}' {
            d -= 1;
        }
    }
    d
}

fn flatten_dedup(src: &str) -> (String, usize) {
    use std::collections::HashSet;
    let mut seen: HashSet<String> = HashSet::new();
    let mut structs: HashSet<String> = HashSet::new();
    let mut depth: i32 = 0;
    let mut dropped = 0usize;
    let mut skip_struct = false; // inside a duplicate struct block
    let mut out = String::with_capacity(src.len());
    for line in src.lines() {
        let t = line.trim_start();
        let mut drop_line = false;
        if skip_struct {
            drop_line = true;
        } else if depth == 0 {
            if let Some(name) = t.strip_prefix("struct ").and_then(|r| r.split_whitespace().next()) {
                let name = name.trim_end_matches('{').to_string();
                if !structs.insert(name) {
                    drop_line = true;
                    // single-line `struct X { ... };` closes immediately; else enter skip mode
                    if !(t.contains('}') && t.contains(';')) {
                        skip_struct = true;
                    }
                }
            } else {
                let is_decl = t.starts_with("uniform ")
                    || t.starts_with("const ")
                    || t.starts_with("in ")
                    || t.starts_with("out ")
                    || t.starts_with("flat in ")
                    || t.starts_with("flat out ");
                if is_decl && t.contains(';') && !t.contains('{') {
                    if let Some(n) = line_decl_name(t) {
                        if !seen.insert(n) {
                            drop_line = true;
                        }
                    }
                }
            }
        }
        // Count the ORIGINAL line's braces always (dropped struct blocks stay balanced),
        // comment-aware so `// [dedup]` output and GLSL comments never skew the depth.
        depth += brace_delta(line);
        if skip_struct && drop_line && depth == 0 && line.contains('}') {
            skip_struct = false; // duplicate struct closed
        }
        if drop_line {
            out.push_str("// [dedup] ");
            dropped += 1;
        }
        out.push_str(line);
        out.push('\n');
    }
    (out, dropped)
}

/// Sweep transform for the VULKAN-VALIDITY gate: value uniforms -> one anonymous std140 block;
/// samplers KEPT COMBINED (legal in Vulkan; the separate-split is a wgpu-consumption constraint,
/// proven on softenLight in H3b3d) with explicit layout(set,binding); initializer uniforms ->
/// const; named blocks get bindings. Returns (src, ubo_members, samplers).
fn vulkan_transform(src: &str) -> (String, usize, usize) {
    let mut members: Vec<String> = Vec::new();
    let mut binding: u32 = 1;
    let mut samplers = 0usize;
    let mut depth: i32 = 0;
    let mut body: Vec<String> = Vec::new();
    for line in src.lines() {
        let t = line.trim_start();
        let mut out_line = line.to_string();
        if depth == 0 && t.starts_with("uniform ") {
            let is_opaque = matches!(t.split_whitespace().nth(1), Some(ty) if ty.starts_with("sampler") || ty.starts_with("isampler") || ty.starts_with("usampler") || ty.starts_with("image"));
            if is_opaque {
                out_line = format!("layout(set = 0, binding = {binding}) {t}");
                binding += 1;
                samplers += 1;
            } else if t.contains('=') {
                out_line = line.replacen("uniform", "const", 1);
            } else if t.contains(';') {
                members.push(t.strip_prefix("uniform ").unwrap().trim().to_string());
                out_line = format!("// [ubo] {t}");
            } else {
                out_line = format!("layout(set = 0, binding = {binding}) {t}");
                binding += 1;
            }
        } else if depth == 0 && t.starts_with("layout") && t.contains("uniform") && !t.contains(';') && !t.contains("binding") {
            out_line = format!("layout(set = 0, binding = {binding}) {t}");
            binding += 1;
        }
        body.push(out_line);
        depth += brace_delta(line); // comment-aware (dedup output contains commented braces)
    }
    let mut block = String::new();
    if !members.is_empty() {
        block.push_str("layout(std140, set = 0, binding = 0) uniform SweepBlock {\n");
        for m in &members {
            block.push_str("    ");
            block.push_str(m);
            block.push('\n');
        }
        block.push_str("};\n");
    }
    // Inject the block at the first non-preprocessor line (post-preprocess there are no
    // piece markers; earliest legal spot = right after the #version/#extension prologue).
    let mut out = String::with_capacity(src.len() + block.len());
    let mut injected = members.is_empty();
    for l in body {
        let t = l.trim_start();
        if !injected && !t.is_empty() && !t.starts_with('#') && !t.starts_with("//") {
            out.push_str(&block);
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

// ---- A2: corpus-wide separate-sampler split (the wgpu-consumption shape) ---------
// Combined samplers are legal Vulkan but wgpu has no combined binding type. Split mode
// produces the wgpu-ready corpus: global `uniform samplerX s;` -> texture+sampler pair +
// `#define s samplerX(s_tex, s_smp)` (constructor at point of use -- legal), PLUS the part
// the #define can't do: functions taking sampler-typed PARAMS get their signatures
// pair-expanded, their call sites pair-expanded (constructors are illegal as call args),
// and in-body param uses reconstructed. Measured surface: 7 files / ~11 signatures, all
// single-line, no sampler arrays (shadowUtil pcf*, screenSpaceReflUtil tap/trace, dof, rlv).

fn split_sampler_ty(st: &str) -> (String, String) {
    let smp = if st.contains("Shadow") { "samplerShadow" } else { "sampler" };
    let (pfx, rest) = if let Some(r) = st.strip_prefix("isampler") {
        ("itexture", r)
    } else if let Some(r) = st.strip_prefix("usampler") {
        ("utexture", r)
    } else {
        ("texture", st.strip_prefix("sampler").unwrap_or(st))
    };
    (format!("{pfx}{}", rest.replace("Shadow", "")), smp.to_string())
}

fn is_sampler_ty(t: &str) -> bool {
    t.starts_with("sampler") || t.starts_with("isampler") || t.starts_with("usampler")
}

/// Replace whole-word occurrences of `name` (not preceded by '.' or '_'-joined) via `f`.
fn replace_ident(text: &str, name: &str, with: &str) -> String {
    let b = text.as_bytes();
    let mut out = String::with_capacity(text.len());
    let mut i = 0;
    while let Some(pos) = text[i..].find(name) {
        let s = i + pos;
        let e = s + name.len();
        let before_ok = s == 0 || {
            let c = b[s - 1] as char;
            !(c.is_alphanumeric() || c == '_' || c == '.')
        };
        let after_ok = e >= b.len() || {
            let c = b[e] as char;
            !(c.is_alphanumeric() || c == '_')
        };
        out.push_str(&text[i..s]);
        if before_ok && after_ok {
            out.push_str(with);
        } else {
            out.push_str(name);
        }
        i = e;
    }
    out.push_str(&text[i..]);
    out
}

/// Pair-expand bare sampler identifiers appearing as top-level args in calls to `known` fns.
/// Works on the whole text (calls may span lines); recurses into nested args.
fn expand_call_args(text: &str, known: &std::collections::HashMap<String, ()>, entities: &std::collections::HashMap<String, String>) -> String {
    let mut out = String::with_capacity(text.len());
    let b = text.as_bytes();
    let mut i = 0usize;
    while i < b.len() {
        // find an identifier start
        let c = b[i] as char;
        if c.is_alphabetic() || c == '_' {
            let start = i;
            while i < b.len() && ((b[i] as char).is_alphanumeric() || b[i] == b'_') {
                i += 1;
            }
            let ident = &text[start..i];
            // skip whitespace to see if a call follows
            let mut j = i;
            while j < b.len() && (b[j] == b' ' || b[j] == b'\t') {
                j += 1;
            }
            let prev_ok = start == 0 || (b[start - 1] as char) != '.';
            if prev_ok && j < b.len() && b[j] == b'(' && known.contains_key(ident) {
                // balanced-paren arg span
                let args_start = j + 1;
                let mut depth = 1i32;
                let mut k = args_start;
                while k < b.len() && depth > 0 {
                    match b[k] {
                        b'(' => depth += 1,
                        b')' => depth -= 1,
                        _ => {}
                    }
                    k += 1;
                }
                let args_end = k - 1; // at ')'
                let inner = expand_call_args(&text[args_start..args_end], known, entities);
                // split top-level commas, expand bare sampler-entity idents
                let mut rebuilt: Vec<String> = Vec::new();
                let (mut d, mut last) = (0i32, 0usize);
                let ib = inner.as_bytes();
                for (idx, &ch) in ib.iter().enumerate() {
                    match ch {
                        b'(' | b'[' => d += 1,
                        b')' | b']' => d -= 1,
                        b',' if d == 0 => {
                            rebuilt.push(inner[last..idx].to_string());
                            last = idx + 1;
                        }
                        _ => {}
                    }
                }
                rebuilt.push(inner[last..].to_string());
                let expanded: Vec<String> = rebuilt
                    .into_iter()
                    .map(|a| {
                        let bare = a.trim();
                        if entities.contains_key(bare) {
                            let pad = &a[..a.len() - a.trim_start().len()];
                            format!("{pad}{bare}_tex, {bare}_smp")
                        } else {
                            a
                        }
                    })
                    .collect();
                out.push_str(ident);
                out.push_str(&text[i..j]); // preserved whitespace
                out.push('(');
                out.push_str(&expanded.join(","));
                out.push(')');
                i = k;
            } else {
                out.push_str(ident);
            }
        } else {
            out.push(c);
            i += 1;
        }
    }
    out
}

/// Join multi-line function SIGNATURES into single logical lines (comment-stripped), so the
/// signature passes see them whole. FXAA's FxaaPixelShader (~20 params, per-param comments),
/// SMAA's sigs, and terrain utils all span lines -- the round-1 single-line assumption missed
/// them (sig unrewritten while call sites/bodies were => both A2 failure shapes). Only lines
/// whose head is exactly `TYPE IDENT (` with unbalanced parens are joined -- statements like
/// `vec4 x = fn(` have a 3-token head and are left alone (calls are handled whole-text in P3).
fn join_signatures(src: &str) -> String {
    let code_of = |l: &str| l.split("//").next().unwrap_or("").to_string();
    let paren_bal = |l: &str| {
        let mut d = 0i32;
        for c in code_of(l).chars() {
            if c == '(' {
                d += 1;
            } else if c == ')' {
                d -= 1;
            }
        }
        d
    };
    let mut out = String::with_capacity(src.len());
    let mut lines = src.lines().peekable();
    while let Some(line) = lines.next() {
        let t = line.trim_start();
        let joinable = !t.starts_with('#') && !t.starts_with("//") && t.contains('(') && {
            match t.find('(') {
                Some(op) => {
                    let head: Vec<&str> = t[..op].split_whitespace().collect();
                    head.len() == 2
                        && head[0].chars().next().is_some_and(|c| c.is_alphabetic())
                        && head[1].chars().all(|c| c.is_alphanumeric() || c == '_')
                }
                None => false,
            }
        };
        if joinable && paren_bal(line) > 0 {
            let mut joined = code_of(line).trim_end().to_string();
            let mut bal = paren_bal(line);
            for cont in lines.by_ref() {
                let piece = code_of(cont);
                joined.push(' ');
                joined.push_str(piece.trim());
                bal += paren_bal(cont);
                if bal <= 0 {
                    break;
                }
            }
            out.push_str(&joined);
            out.push('\n');
        } else {
            out.push_str(line);
            out.push('\n');
        }
    }
    out
}

/// The A2 split transform: like vulkan_transform but samplers become separate pairs, with
/// function-signature + call-site + body rewriting. Returns (src, ubo_members, samplers).
fn split_transform(input: &str) -> (String, usize, usize) {
    use std::collections::HashMap;
    let src = &join_signatures(input);
    // P1: collect sampler-param fn signatures (defs + prototypes; measured all single-line)
    let mut known: HashMap<String, ()> = HashMap::new();
    let mut fn_params: HashMap<String, Vec<(String, String)>> = HashMap::new(); // fn -> [(type,name)]
    for line in src.lines() {
        let t = line.trim_start();
        if t.starts_with('#') || t.starts_with("//") || !t.contains('(') || !t.contains("sampler") {
            continue;
        }
        let Some(op) = t.find('(') else { continue };
        let Some(cp) = t.rfind(')') else { continue };
        if cp < op {
            continue;
        }
        let head: Vec<&str> = t[..op].split_whitespace().collect();
        // "RET name(" -- reject control flow / calls (head must be exactly type + ident)
        if head.len() != 2 || !head[0].chars().next().is_some_and(|c| c.is_alphabetic()) {
            continue;
        }
        let fname = head[1];
        let mut sam_params = Vec::new();
        for p in t[op + 1..cp].split(',') {
            let toks: Vec<&str> = p.split_whitespace().collect();
            if toks.len() >= 2 && is_sampler_ty(toks[toks.len() - 2]) {
                sam_params.push((toks[toks.len() - 2].to_string(), toks[toks.len() - 1].to_string()));
            }
        }
        if !sam_params.is_empty() {
            known.insert(fname.to_string(), ());
            fn_params.insert(fname.to_string(), sam_params);
        }
    }

    // P2: line pass -- UBO/base transform + global sampler split + signature rewrite
    let mut members: Vec<String> = Vec::new();
    let mut binding: u32 = 1;
    let mut n_samplers = 0usize;
    let mut globals: HashMap<String, String> = HashMap::new(); // name -> type
    let mut depth: i32 = 0;
    let mut body: Vec<String> = Vec::new();
    for line in src.lines() {
        let t = line.trim_start();
        let mut out_line = line.to_string();
        if depth == 0 && t.starts_with("uniform ") {
            let ty = t.split_whitespace().nth(1).unwrap_or("");
            if is_sampler_ty(ty) || ty.starts_with("image") {
                let name = line_decl_name(t).unwrap_or_default();
                let (tex_ty, smp_ty) = split_sampler_ty(ty);
                let (b_tex, b_smp) = (binding, binding + 1);
                binding += 2;
                n_samplers += 1;
                globals.insert(name.clone(), ty.to_string());
                out_line = format!(
                    "layout(set = 0, binding = {b_tex}) uniform {tex_ty} {name}_tex;\n\
                     layout(set = 0, binding = {b_smp}) uniform {smp_ty} {name}_smp;\n\
                     #define {name} {ty}({name}_tex, {name}_smp)"
                );
            } else if t.contains('=') {
                out_line = line.replacen("uniform", "const", 1);
            } else if t.contains(';') {
                members.push(t.strip_prefix("uniform ").unwrap().trim().to_string());
                out_line = format!("// [ubo] {t}");
            } else {
                out_line = format!("layout(set = 0, binding = {binding}) {t}");
                binding += 1;
            }
        } else if depth == 0 && t.starts_with("layout") && t.contains("uniform") && !t.contains(';') && !t.contains("binding") {
            out_line = format!("layout(set = 0, binding = {binding}) {t}");
            binding += 1;
        } else if !t.starts_with('#') && !t.starts_with("//") && t.contains('(') && t.contains("sampler") {
            // signature rewrite (defs + prototypes): pair-expand sampler params
            if let (Some(op), Some(cp)) = (t.find('('), t.rfind(')')) {
                let head: Vec<&str> = t[..op].split_whitespace().collect();
                if cp > op && head.len() == 2 && known.contains_key(head[1]) {
                    let rebuilt: Vec<String> = t[op + 1..cp]
                        .split(',')
                        .map(|p| {
                            let toks: Vec<&str> = p.split_whitespace().collect();
                            if toks.len() >= 2 && is_sampler_ty(toks[toks.len() - 2]) {
                                let (tex_ty, smp_ty) = split_sampler_ty(toks[toks.len() - 2]);
                                let n = toks[toks.len() - 1];
                                format!("{tex_ty} {n}_tex, {smp_ty} {n}_smp")
                            } else {
                                p.trim().to_string()
                            }
                        })
                        .collect();
                    let indent = &line[..line.len() - t.len()];
                    out_line = format!("{indent}{} {}({}){}", head[0], head[1], rebuilt.join(", "), &t[cp + 1..]);
                }
            }
        }
        body.push(out_line);
        depth += brace_delta(line);
    }

    // SweepBlock (same as vulkan_transform)
    let mut block = String::new();
    if !members.is_empty() {
        block.push_str("layout(std140, set = 0, binding = 0) uniform SweepBlock {\n");
        for m in &members {
            block.push_str("    ");
            block.push_str(m);
            block.push('\n');
        }
        block.push_str("};\n");
    }
    let mut text = String::with_capacity(src.len() + block.len());
    let mut injected = members.is_empty();
    for l in &body {
        let t = l.trim_start();
        if !injected && !t.is_empty() && !t.starts_with('#') && !t.starts_with("//") {
            text.push_str(&block);
            injected = true;
        }
        text.push_str(l);
        text.push('\n');
    }
    if !injected {
        text.push_str(&block);
    }

    // P3: pair-expand bare sampler args in calls to known fns (params + globals in scope).
    // Entities map = globals + every known fn's params (over-broad but names are unique).
    let mut entities: HashMap<String, String> = globals.clone();
    for ps in fn_params.values() {
        for (ty, n) in ps {
            entities.insert(n.clone(), ty.clone());
        }
    }
    let text = expand_call_args(&text, &known, &entities);

    // P4: inside each known fn body, reconstruct remaining bare param uses at point of use.
    // Find the (rewritten) def line "RET name(...) " and rewrite until brace close.
    let mut out = String::with_capacity(text.len());
    let mut lines = text.lines().peekable();
    while let Some(line) = lines.next() {
        let t = line.trim_start();
        let mut is_def = None;
        if !t.starts_with('#') && !t.starts_with("//") && t.contains('(') {
            if let Some(op) = t.find('(') {
                let head: Vec<&str> = t[..op].split_whitespace().collect();
                if head.len() == 2 && known.contains_key(head[1]) && t.contains(')') && !t.trim_end().ends_with(';') {
                    is_def = Some(head[1].to_string());
                }
            }
        }
        out.push_str(line);
        out.push('\n');
        if let Some(fname) = is_def {
            let params = fn_params.get(&fname).cloned().unwrap_or_default();
            let mut d = brace_delta(line);
            let started = d > 0;
            let mut waiting = !started; // brace on a later line
            for bl in lines.by_ref() {
                let mut rl = bl.to_string();
                for (ty, n) in &params {
                    rl = replace_ident(&rl, n, &format!("{ty}({n}_tex, {n}_smp)"));
                }
                out.push_str(&rl);
                out.push('\n');
                d += brace_delta(bl);
                if waiting && d > 0 {
                    waiting = false;
                }
                if !waiting && d <= 0 {
                    break;
                }
            }
        }
    }
    (out, members.len(), n_samplers)
}

/// Run the shaderc preprocessor over the assembled TU (defines are in the source). This resolves
/// #if branches BEFORE dedup -- the viewer's per-object compiles preprocessed first too, so
/// textual dedup on unpreprocessed source drops decls the preprocessor would have kept
/// (e.g. terrain's per-detail-level re-declarations under mutually-exclusive #if).
fn preprocess(src: &str, fragment: bool, name: &str) -> Result<String, String> {
    let compiler = shaderc::Compiler::new().expect("shaderc");
    let mut opts = shaderc::CompileOptions::new().expect("shaderc opts");
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_3 as u32);
    let kind = if fragment { shaderc::ShaderKind::Fragment } else { shaderc::ShaderKind::Vertex };
    compiler
        .preprocess(src, name, "main", Some(&opts))
        .map(|a| a.as_text())
        .map_err(|e| e.to_string())
}

// ---- canonical shader levels (llviewershadermgr.cpp:613-636) ---------------------
const LVL_LIGHTING: i32 = 3;
const LVL_ENVIRONMENT: i32 = 2;
const LVL_WINDLIGHT: i32 = 2;
const LVL_WATER: i32 = 3;
const LVL_DEFERRED: i32 = 3;
const LVL_OBJECT: i32 = 2;
const LVL_EFFECT: i32 = 2;
const LVL_INTERFACE: i32 = 2;
const LVL_AVATAR: i32 = 1;
const SUM_LIGHTS_CLASS: i32 = 3;
const INDEXED_CHANNELS: i32 = 4; // LLGLSLShader::sIndexedTextureChannels (hardcoded 4)

/// The feature flags attachShaderFeatures consults (llshadermgr.cpp:68-366).
#[derive(Default, Clone)]
pub struct Features {
    pub calculates_lighting: bool,
    pub calculates_atmospherics: bool,
    pub has_lighting: bool,
    pub is_alpha_lighting: bool,
    pub has_atmospherics: bool,
    pub has_gamma: bool,
    pub has_srgb: bool,
    pub is_deferred: bool,
    pub has_full_gbuffer: bool,
    pub has_screen_space_reflections: bool,
    pub has_reflection_probes: bool,
    pub has_shadows: bool,
    pub has_ambient_occlusion: bool,
    pub is_specular: bool,
    pub has_skinning: bool,
    pub has_object_skinning: bool,
    pub has_alpha_mask: bool,
    pub is_pbr_terrain: bool,
    pub has_tonemap: bool,
    pub attach_nothing: bool,
    pub indexed_texture_channels: i32, // mIndexedTextureChannels for the program mains
}

pub struct ProgramDef {
    pub name: &'static str,
    pub level: i32,
    pub features: Features,
    /// per-program permutations (addPermutation) -- unioned with the canonical globals
    pub defines: Vec<(&'static str, String)>,
    pub vert: Vec<&'static str>,
    pub frag: Vec<&'static str>,
}

/// Canonical global defines (loadBasicShaders attribs, llviewershadermgr.cpp:806-863).
fn global_defines() -> Vec<(&'static str, String)> {
    vec![
        ("MAX_JOINTS_PER_MESH_OBJECT", "110".into()),
        ("SUN_SHADOW", "1".into()),
        ("SPOT_SHADOW", "1".into()),
        ("REFMAP_LEVEL", "3".into()),
        ("REF_SAMPLE_COUNT", "32".into()),
        ("TERRAIN_PLANAR_TEXTURE_SAMPLE_COUNT", "3".into()),
        ("TERRAIN_TRIPLANAR_BLEND_FACTOR", "4.00".into()),
        ("TERRAIN_PBR_DETAIL", "0".into()),
        // GLTF caps from canonical mMaxUniformBlockSize=65536 (llviewershadermgr.cpp:291-302)
        ("MAX_NODES_PER_GLTF_OBJECT", "1365".into()),
        ("MAX_MATERIALS_PER_GLTF_OBJECT", "341".into()),
        ("MAX_UBO_VEC4S", "4096".into()),
    ]
}

/// attachShaderFeatures, vertex stage: (condition, file, level) in ATTACH ORDER
/// (llshadermgr.cpp:82-180 + the trailing V12 at :350-363). Levels per loadBasicShaders.
fn vertex_attaches(f: &Features) -> Vec<(&'static str, i32)> {
    let mut v = Vec::new();
    if f.attach_nothing {
        return v;
    }
    if f.calculates_atmospherics || f.has_gamma || f.is_deferred {
        v.push(("windlight/atmosphericsVarsV.glsl", LVL_WINDLIGHT));
    }
    if f.calculates_lighting || f.calculates_atmospherics {
        v.push(("windlight/atmosphericsHelpersV.glsl", LVL_WINDLIGHT));
    }
    if f.calculates_lighting {
        if f.is_specular {
            v.push(("lighting/lightFuncSpecularV.glsl", LVL_LIGHTING));
            if !f.is_alpha_lighting {
                v.push(("lighting/sumLightsSpecularV.glsl", SUM_LIGHTS_CLASS));
            }
            v.push(("lighting/lightSpecularV.glsl", LVL_LIGHTING));
        } else {
            v.push(("lighting/lightFuncV.glsl", LVL_LIGHTING));
            if !f.is_alpha_lighting {
                v.push(("lighting/sumLightsV.glsl", SUM_LIGHTS_CLASS));
            }
            v.push(("lighting/lightV.glsl", LVL_LIGHTING));
        }
    }
    if f.calculates_atmospherics {
        v.push(("environment/srgbF.glsl", 1)); // "F" file attached to VERTEX stage, level 1
        v.push(("windlight/atmosphericsFuncs.glsl", LVL_WINDLIGHT));
        v.push(("windlight/atmosphericsV.glsl", LVL_WINDLIGHT));
    }
    if f.has_skinning {
        v.push(("avatar/avatarSkinV.glsl", 1));
    }
    if f.has_object_skinning {
        v.push(("avatar/objectSkinV.glsl", 1));
    }
    v.push(("deferred/textureUtilV.glsl", 1)); // unconditional
    // trailing vertex attach (emitted after the fragment attaches in C++; stage-sorted here)
    if f.indexed_texture_channels <= 1 {
        v.push(("objects/nonindexedTextureV.glsl", 1));
    } else {
        v.push(("objects/indexedTextureV.glsl", 1));
    }
    v
}

/// attachShaderFeatures, fragment stage (llshadermgr.cpp:188-348).
fn fragment_attaches(f: &Features) -> Vec<(&'static str, i32)> {
    let mut v = Vec::new();
    if f.attach_nothing {
        return v;
    }
    v.push(("deferred/globalF.glsl", 1)); // unconditional
    if f.has_srgb || f.has_atmospherics || f.calculates_atmospherics || f.is_deferred {
        v.push(("environment/srgbF.glsl", LVL_ENVIRONMENT));
    }
    if f.calculates_atmospherics || f.has_gamma || f.is_deferred {
        v.push(("windlight/atmosphericsVarsF.glsl", LVL_WINDLIGHT));
    }
    if f.calculates_lighting || f.calculates_atmospherics {
        v.push(("windlight/atmosphericsHelpersF.glsl", LVL_WINDLIGHT));
    }
    if f.is_deferred || f.has_reflection_probes {
        v.push(("deferred/deferredUtil.glsl", 1));
    }
    if f.has_full_gbuffer {
        v.push(("deferred/gbufferUtil.glsl", 1));
    }
    if f.has_screen_space_reflections || f.has_reflection_probes {
        v.push(("deferred/screenSpaceReflUtil.glsl", if cfg().ssr { 3 } else { 1 }));
    }
    if f.has_shadows {
        v.push(("deferred/shadowUtil.glsl", 1));
    }
    if f.has_reflection_probes {
        v.push(("deferred/reflectionProbeF.glsl", if cfg().probes { 3 } else { 2 }));
    }
    if f.has_ambient_occlusion {
        v.push(("deferred/aoUtil.glsl", 1));
    }
    if f.has_gamma || f.is_deferred {
        v.push(("windlight/gammaF.glsl", LVL_WINDLIGHT));
    }
    if f.has_atmospherics || f.is_deferred {
        v.push(("windlight/atmosphericsFuncs.glsl", LVL_WINDLIGHT));
        v.push(("windlight/atmosphericsF.glsl", LVL_WINDLIGHT));
    }
    if f.is_pbr_terrain {
        v.push(("deferred/pbrterrainUtilF.glsl", 1));
    }
    if f.has_tonemap {
        v.push(("deferred/tonemapUtilF.glsl", 1));
    }
    if f.has_atmospherics {
        v.push(("environment/waterFogF.glsl", LVL_WATER));
    }
    if f.has_lighting {
        if f.indexed_texture_channels <= 1 {
            if f.has_alpha_mask {
                v.push(("lighting/lightAlphaMaskNonIndexedF.glsl", LVL_LIGHTING));
            } else {
                v.push(("lighting/lightNonIndexedF.glsl", LVL_LIGHTING));
            }
        } else if f.has_alpha_mask {
            v.push(("lighting/lightAlphaMaskF.glsl", LVL_LIGHTING));
        } else {
            v.push(("lighting/lightF.glsl", LVL_LIGHTING));
        }
    }
    v
}

/// Class-resolution walk (llshadermgr.cpp:516-544): class<level> down to class1, first hit.
fn resolve(file: &str, level: i32) -> Option<(String, String)> {
    let (sub, name) = file.split_once('/')?;
    let mut cls = level.max(1);
    while cls > 0 {
        let p = format!("{SHADER_ROOT}/class{cls}/{sub}/{name}");
        if std::path::Path::new(&p).exists() {
            return Some((format!("class{cls}"), p));
        }
        cls -= 1;
    }
    None
}

/// The viewer's diffuseLookup emission (llshadermgr.cpp:656-748), switch form (non-NVIDIA),
/// fragment stage only, channels = mIndexedTextureChannels.
fn diffuse_lookup_block(channels: i32) -> String {
    let mut s = String::new();
    s.push_str("#define HAS_DIFFUSE_LOOKUP\n");
    for i in 0..channels {
        s.push_str(&format!("uniform sampler2D tex{i};\n"));
    }
    if channels > 1 {
        s.push_str("flat in int vary_texture_index;\n");
    }
    s.push_str("vec4 diffuseLookup(vec2 texcoord)\n{\n");
    if channels == 1 {
        s.push_str("return texture(tex0, texcoord);\n}\n");
    } else {
        s.push_str("\tvec4 ret = vec4(1,0,1,1);\n\tswitch (vary_texture_index)\n\t{\n");
        for i in 0..channels {
            s.push_str(&format!("\t\tcase {i}: return texture(tex{i}, texcoord);\n"));
        }
        s.push_str("\t}\n\treturn ret;\n}\n");
    }
    s
}

/// Assemble one stage of one program into a single flattened TU.
pub fn assemble(p: &ProgramDef, fragment: bool) -> (String, Vec<String>) {
    let mut missing = Vec::new();
    // union of canonical globals + per-program permutations, lexicographic (D7)
    // program permutations WIN over canonical globals on collision (more specific source)
    let mut defines: Vec<(String, String)> = p.defines.iter().map(|(k, v)| (k.to_string(), v.clone())).collect();
    for (k, v) in global_defines() {
        if cfg().remove.contains(&k) { continue; }
        if !defines.iter().any(|(dk, _)| dk == k) {
            defines.push((k.to_string(), v));
        }
    }
    for (k, v) in cfg().add {
        if !defines.iter().any(|(dk, _)| dk == *k) { defines.push((k.to_string(), v.to_string())); }
    }
    defines.sort();

    // gather bodies first so #extension lines can be hoisted (D4 superset)
    let attaches = if fragment { fragment_attaches(&p.features) } else { vertex_attaches(&p.features) };
    let mains: Vec<(&str, i32)> = (if fragment { &p.frag } else { &p.vert })
        .iter()
        .map(|f| (*f, p.level))
        .collect();
    let mut extensions: Vec<String> = Vec::new();
    let mut pieces: Vec<(String, String)> = Vec::new(); // (label, body)
    for (file, lvl) in attaches.iter().chain(mains.iter()) {
        match resolve(file, *lvl) {
            Some((cls, path)) => {
                let raw = std::fs::read_to_string(&path).unwrap_or_default();
                let mut body = String::with_capacity(raw.len());
                for line in raw.lines() {
                    let t = line.trim_start();
                    if t.contains("[EXTRA_CODE_HERE]") {
                        continue; // marker line dropped (viewer drops it too)
                    }
                    if t.starts_with("#extension") {
                        if !extensions.contains(&t.to_string()) {
                            extensions.push(t.to_string());
                        }
                        continue; // hoisted to TU top
                    }
                    body.push_str(line);
                    body.push('\n');
                }
                pieces.push((format!("{file} [{cls}]"), body));
            }
            None => missing.push(file.to_string()),
        }
    }

    // preamble in the viewer's emission order (loadShaderFile)
    let mut s = String::new();
    s.push_str("#version 450\n");
    for e in &extensions {
        s.push_str(e);
        s.push('\n');
    }
    s.push_str("precision highp float;\nprecision highp int;\n");
    s.push_str(if fragment { "#define FRAGMENT_SHADER 1\n" } else { "#define VERTEX_SHADER 1\n" });
    s.push_str("#define GBUFFER_FLAG_SKIP_ATMOS 0.0\n#define GBUFFER_FLAG_HAS_ATMOS 0.34\n");
    s.push_str("#define GBUFFER_FLAG_HAS_PBR 0.67\n#define GBUFFER_FLAG_HAS_HDRI 1.0\n");
    s.push_str("#define GET_GBUFFER_FLAG(data, flag) (abs(data-flag)< 0.1)\n");
    for (k, v) in &defines {
        s.push_str(&format!("#define {k} {v}\n"));
    }
    s.push_str("#define IS_AMD_CARD 1\n");
    if fragment && p.features.indexed_texture_channels > 0 {
        s.push_str(&diffuse_lookup_block(p.features.indexed_texture_channels));
    }
    s.push_str("struct GBufferInfo { vec4 albedo; vec4 specular; vec3 normal; vec4 emissive; float gbufferFlag; float envIntensity; };\n");
    for (label, body) in &pieces {
        s.push_str(&format!("\n// ===== {label} =====\n"));
        s.push_str(body);
    }
    (s, missing)
}

// ---- the program table (starter slice; llviewershadermgr.cpp anchors in comments) ----
// Encoded as data with the C++'s straight-line assignments mirrored. Loop-generated
// families (Material[32], FXAA/SMAA[4], GLTF variants, rigged variants) come next.

fn deferred_features() -> Features {
    Features { has_srgb: true, is_deferred: true, ..Default::default() }
}

pub fn programs() -> Vec<ProgramDef> {
    let mut v = Vec::new();

    // gDeferredDiffuseProgram (:1232) -- indexed diffuse into the G-buffer
    v.push(ProgramDef {
        name: "deferred_diffuse",
        level: LVL_DEFERRED,
        features: Features { has_srgb: true, indexed_texture_channels: INDEXED_CHANNELS, ..Default::default() },
        defines: vec![],
        vert: vec!["deferred/diffuseV.glsl"],
        frag: vec!["deferred/diffuseIndexedF.glsl"],
    });

    // gDeferredSoftenProgram (:2134) -- the sun/lighting resolve (heaviest surface)
    v.push(ProgramDef {
        name: "deferred_soften",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true, calculates_atmospherics: true, has_atmospherics: true, has_gamma: true,
            is_deferred: true, has_full_gbuffer: true, has_shadows: true, has_reflection_probes: true,
            ..Default::default()
        },
        defines: vec![("HAS_SUN_SHADOW", "1".into()), ("HAS_SSAO", "1".into())],
        vert: vec!["deferred/softenLightV.glsl"],
        frag: vec!["deferred/softenLightF.glsl"],
    });

    // gDeferredWLSkyProgram (:2929) -- windlight sky
    v.push(ProgramDef {
        name: "deferred_wl_sky",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true, has_atmospherics: true, has_gamma: true, has_srgb: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/skyV.glsl"],
        frag: vec!["deferred/skyF.glsl"],
    });

    // gObjectAlphaMaskNoColorProgram (:3113) -- forward object/simple
    v.push(ProgramDef {
        name: "object_alphamask_nocolor",
        level: LVL_OBJECT,
        features: Features {
            calculates_lighting: true, calculates_atmospherics: true, has_gamma: true,
            has_atmospherics: true, has_lighting: true, has_alpha_mask: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["objects/simpleNoColorV.glsl"],
        frag: vec!["objects/simpleF.glsl"],
    });

    // gPostVignetteProgram (:1070) -- post, no features
    v.push(ProgramDef {
        name: "post_vignette",
        level: LVL_EFFECT,
        features: Features::default(),
        defines: vec![],
        vert: vec!["post/exoPostBaseV.glsl"],
        frag: vec!["post/exoVignetteF.glsl"],
    });

    // gDeferredShadowProgram-alike (sun shadow caster pass)
    v.push(ProgramDef {
        name: "deferred_shadow",
        level: LVL_DEFERRED,
        features: deferred_features(),
        defines: vec![],
        vert: vec!["deferred/shadowV.glsl"],
        frag: vec!["deferred/shadowF.glsl"],
    });

    // gDeferredTerrainProgram (PBR terrain surface)
    v.push(ProgramDef {
        name: "deferred_pbr_terrain",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true, is_deferred: true, is_pbr_terrain: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/pbrterrainV.glsl"],
        frag: vec!["deferred/pbrterrainF.glsl"],
    });

    // gDeferredAvatarProgram -- avatar into G-buffer (skinning)
    v.push(ProgramDef {
        name: "deferred_avatar",
        level: LVL_DEFERRED,
        features: Features {
            has_srgb: true, is_deferred: true, has_skinning: true,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/avatarV.glsl"],
        frag: vec!["deferred/avatarF.glsl"],
    });

    // gDeferredFullbrightProgram -- fullbright (indexed)
    v.push(ProgramDef {
        name: "deferred_fullbright",
        level: LVL_DEFERRED,
        features: Features {
            calculates_atmospherics: true, has_gamma: true, has_atmospherics: true, has_srgb: true,
            indexed_texture_channels: INDEXED_CHANNELS,
            ..Default::default()
        },
        defines: vec![],
        vert: vec!["deferred/fullbrightV.glsl"],
        frag: vec!["deferred/fullbrightF.glsl"],
    });

    // gDeferredAlphaProgram-alike (the deferred alpha forward pass; heavy feature set)
    v.push(ProgramDef {
        name: "deferred_alpha",
        level: LVL_DEFERRED,
        features: Features {
            calculates_lighting: false, has_lighting: false, is_alpha_lighting: true,
            calculates_atmospherics: true, has_gamma: true, has_atmospherics: true, has_srgb: true,
            is_deferred: true, has_shadows: true, has_reflection_probes: true,
            indexed_texture_channels: INDEXED_CHANNELS,
            ..Default::default()
        },
        defines: vec![
            ("USE_INDEXED_TEX", "1".into()),
            ("HAS_SUN_SHADOW", "1".into()),
            ("USE_VERTEX_COLOR", "1".into()),
        ],
        vert: vec!["deferred/alphaV.glsl"],
        frag: vec!["deferred/alphaF.glsl"],
    });

    v
}

// ---- compile + validate + scoreboard --------------------------------------------

fn compile_vulkan(src: &str, fragment: bool, name: &str) -> Result<Vec<u32>, String> {
    let compiler = shaderc::Compiler::new().expect("shaderc");
    let mut opts = shaderc::CompileOptions::new().expect("shaderc opts");
    opts.set_target_env(shaderc::TargetEnv::Vulkan, shaderc::EnvVersion::Vulkan1_3 as u32);
    opts.set_auto_map_locations(true);
    let kind = if fragment { shaderc::ShaderKind::Fragment } else { shaderc::ShaderKind::Vertex };
    compiler
        .compile_into_spirv(src, kind, name, "main", Some(&opts))
        .map(|a| a.as_binary().to_vec())
        .map_err(|e| e.to_string())
}

fn spirv_val(words: &[u32]) -> Result<(), String> {
    let dir = std::env::temp_dir().join("sweep_spv");
    let _ = std::fs::create_dir_all(&dir);
    let p = dir.join("val.spv");
    std::fs::write(&p, bytemuck::cast_slice::<u32, u8>(words)).map_err(|e| e.to_string())?;
    let out = std::process::Command::new("C:/VulkanSDK/1.4.350.0/Bin/spirv-val.exe")
        .arg("--target-env").arg("vulkan1.3")
        .arg(&p)
        .output()
        .map_err(|e| format!("spirv-val spawn: {e}"))?;
    if out.status.success() {
        Ok(())
    } else {
        Err(String::from_utf8_lossy(&out.stderr).trim().to_string())
    }
}

/// `cargo run -- sweep` -- run the Phase 1 gate over the program table.
pub fn run_sweep(split: bool) -> bool {
    let dump = std::env::temp_dir().join(if split { "sweep_fail_split" } else { "sweep_fail" });
    let _ = std::fs::create_dir_all(&dump);
    let table = crate::sweep_table::programs_full();
    let mut total_fail = 0usize;
    for (ci, c) in CONFIGS.iter().enumerate() {
        if split && ci > 0 {
            break; // A2 gate = canonical config; matrix x split is a later cheap add
        }
    CFG_IDX.store(ci, std::sync::atomic::Ordering::Relaxed);
    log::info!("== config: {} ==", c.name);
    let mut pass = 0usize;
    let mut fail = 0usize;
    log::info!("PHASE 1 SWEEP -- {} programs x 2 stages (assemble -> transform -> shaderc Vulkan1.3 -> spirv-val)", table.len());
    for p in &table {
        for &fragment in &[false, true] {
            let stage = if fragment { "frag" } else { "vert" };
            let (assembled, missing) = assemble(p, fragment);
            if !missing.is_empty() {
                log::warn!("  {:<28} {}  MISSING FILES: {:?}", p.name, stage, missing);
                fail += 1;
                continue;
            }
            let pre = match preprocess(&assembled, fragment, p.name) {
                Ok(t) => t,
                Err(e) => {
                    let f = dump.join(format!("{}_{stage}.glsl", p.name));
                    let _ = std::fs::write(&f, &assembled);
                    log::warn!("  {:<28} {}  PREPROCESS FAIL -> {}
    {}", p.name, stage, f.display(), e.lines().next().unwrap_or(""));
                    fail += 1;
                    continue;
                }
            };
            let (deduped, _) = flatten_dedup(&pre);
            let (transformed, n_ubo, n_smp) = if split {
                split_transform(&deduped)
            } else {
                vulkan_transform(&deduped)
            };
            match compile_vulkan(&transformed, fragment, &format!("{}.{stage}", p.name)) {
                Ok(words) => match spirv_val(&words) {
                    Ok(()) => {
                        log::debug!("  {:<28} {}  OK  ({} ubo members, {} samplers, {} words)", p.name, stage, n_ubo, n_smp, words.len());
                        pass += 1;
                    }
                    Err(e) => {
                        let f = dump.join(format!("{}_{stage}.glsl", p.name));
                        let _ = std::fs::write(&f, &transformed);
                        log::warn!("  {:<28} {}  SPIRV-VAL FAIL -> {}\n    {}", p.name, stage, f.display(), e.lines().next().unwrap_or(""));
                        fail += 1;
                    }
                },
                Err(e) => {
                    let f = dump.join(format!("{}_{stage}.glsl", p.name));
                    let _ = std::fs::write(&f, &transformed);
                    let first = e.lines().find(|l| l.contains("error")).unwrap_or_else(|| e.lines().next().unwrap_or(""));
                    log::warn!("  {:<28} {}  COMPILE FAIL -> {}\n    {}", p.name, stage, f.display(), first);
                    fail += 1;
                }
            }
        }
    }
    log::info!("  config {}: {pass} pass / {fail} fail (of {})", c.name, pass + fail);
    total_fail += fail;
    }
    log::info!("SWEEP MATRIX: {} configs -- total failures {}. Dumps in {}", CONFIGS.len(), total_fail, dump.display());
    total_fail == 0
}

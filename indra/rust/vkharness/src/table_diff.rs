//! P2a table-diff: validate the transcribed program table (sweep_table.rs) against a
//! live-viewer manifest produced by the FS_SHADER_MANIFEST dump in createShader
//! (fs/scene-dump branch). Matching is by (ordered per-stage file list) first -- table
//! names are slugs, viewer names are display strings -- then defines select the variant
//! among file-sharing families (Material[32], GLTF[16]). Viewer mDefines = permutations
//! only (globals never enter programs, D1), same namespace as the table's `defines`.
//!
//! Report classes:
//!   MISSING   -- manifest program with no file-match in the table (transcription hole; critical)
//!   DEFINES   -- file-matched but no defines-exact variant (shows nearest candidate delta)
//!   FEAT/LVL  -- matched pair disagrees on a feature flag or shader level
//!   NOT-SEEN  -- table entries the run never created (informational: settings-gated)

use std::collections::BTreeMap;

fn extract_str<'a>(line: &'a str, key: &str) -> Option<&'a str> {
    let pat = format!("\"{key}\":\"");
    let s = line.find(&pat)? + pat.len();
    let e = line[s..].find('"')? + s;
    Some(&line[s..e])
}

fn extract_i32(line: &str, key: &str) -> Option<i32> {
    let pat = format!("\"{key}\":");
    let s = line.find(&pat)? + pat.len();
    let rest = &line[s..];
    let end = rest.find(|c: char| !(c.is_ascii_digit() || c == '-')).unwrap_or(rest.len());
    rest[..end].parse().ok()
}

/// Extract the `[ ["a","b"], ["c","d"] ]` pair-array that FOLLOWS `"key":[`.
fn extract_pairs(line: &str, key: &str) -> Vec<(String, String)> {
    let pat = format!("\"{key}\":[");
    let Some(mut i) = line.find(&pat).map(|p| p + pat.len()) else { return vec![] };
    let b = line.as_bytes();
    let mut out = Vec::new();
    let mut depth = 1i32;
    let mut cur: Vec<String> = Vec::new();
    while i < b.len() && depth > 0 {
        match b[i] {
            b'[' => {
                depth += 1;
                cur.clear();
            }
            b']' => {
                depth -= 1;
                if depth == 1 && cur.len() == 2 {
                    out.push((cur[0].clone(), cur[1].clone()));
                }
            }
            b'"' => {
                let s = i + 1;
                let e = line[s..].find('"').map(|p| p + s).unwrap_or(s);
                cur.push(line[s..e].to_string());
                i = e;
            }
            _ => {}
        }
        i += 1;
    }
    out
}

struct ManifestEntry {
    name: String,
    level: i32,
    vert: Vec<String>,
    frag: Vec<String>,
    defines: BTreeMap<String, String>,
    features: BTreeMap<String, i32>,
}

const FEAT_KEYS: &[&str] = &[
    "idxch", "calculatesLighting", "calculatesAtmospherics", "hasLighting", "isAlphaLighting",
    "isSpecular", "hasSkinning", "hasObjectSkinning", "hasAtmospherics", "hasGamma", "hasShadows",
    "hasAmbientOcclusion", "hasSrgb", "isDeferred", "hasFullGBuffer", "hasScreenSpaceReflections",
    "hasAlphaMask", "hasReflectionProbes", "attachNothing", "isPBRTerrain", "hasTonemap",
];

fn parse_manifest(path: &str) -> Vec<ManifestEntry> {
    let text = std::fs::read_to_string(path).unwrap_or_default();
    let mut out = Vec::new();
    for line in text.lines() {
        if !line.starts_with('{') {
            continue;
        }
        let files = extract_pairs(line, "files");
        let mut e = ManifestEntry {
            name: extract_str(line, "name").unwrap_or("?").to_string(),
            level: extract_i32(line, "level").unwrap_or(-1),
            vert: files.iter().filter(|(_, s)| s == "vert").map(|(f, _)| f.clone()).collect(),
            frag: files.iter().filter(|(_, s)| s == "frag").map(|(f, _)| f.clone()).collect(),
            defines: extract_pairs(line, "defines").into_iter().collect(),
            features: BTreeMap::new(),
        };
        for k in FEAT_KEYS {
            if let Some(v) = extract_i32(line, k) {
                e.features.insert(k.to_string(), v);
            }
        }
        out.push(e);
    }
    out
}

fn table_features(f: &crate::sweep::Features) -> BTreeMap<String, i32> {
    let mut m = BTreeMap::new();
    let mut put = |k: &str, v: bool| {
        m.insert(k.to_string(), v as i32);
    };
    put("calculatesLighting", f.calculates_lighting);
    put("calculatesAtmospherics", f.calculates_atmospherics);
    put("hasLighting", f.has_lighting);
    put("isAlphaLighting", f.is_alpha_lighting);
    put("isSpecular", f.is_specular);
    put("hasSkinning", f.has_skinning);
    put("hasObjectSkinning", f.has_object_skinning);
    put("hasAtmospherics", f.has_atmospherics);
    put("hasGamma", f.has_gamma);
    put("hasShadows", f.has_shadows);
    put("hasAmbientOcclusion", f.has_ambient_occlusion);
    put("hasSrgb", f.has_srgb);
    put("isDeferred", f.is_deferred);
    put("hasFullGBuffer", f.has_full_gbuffer);
    put("hasScreenSpaceReflections", f.has_screen_space_reflections);
    put("hasAlphaMask", f.has_alpha_mask);
    put("hasReflectionProbes", f.has_reflection_probes);
    put("attachNothing", f.attach_nothing);
    put("isPBRTerrain", f.is_pbr_terrain);
    put("hasTonemap", f.has_tonemap);
    m.insert("idxch".to_string(), f.indexed_texture_channels);
    m
}

pub fn run_table_diff(manifest_path: &str) -> bool {
    // Settings-driven defines to normalize away (documented deltas between the manifest
    // run's real settings and the table's canonical settings), via --ignore KEY args.
    let ignore: Vec<String> = std::env::args()
        .skip_while(|x| x != "--ignore")
        .skip(1)
        .take_while(|x| !x.starts_with("--"))
        .collect();
    let ignore_all: Vec<String> = {
        let mut v = ignore;
        let mut i = std::env::args().collect::<Vec<_>>();
        // collect every occurrence of --ignore K
        let mut extra = Vec::new();
        while let Some(p) = i.iter().position(|x| x == "--ignore") {
            if p + 1 < i.len() {
                extra.push(i[p + 1].clone());
            }
            i.drain(..p + 1);
        }
        v.extend(extra);
        v.sort();
        v.dedup();
        v
    };
    if !ignore_all.is_empty() {
        log::info!("table-diff: normalizing away settings-driven defines: {:?}", ignore_all);
    }
    let manifest = parse_manifest(manifest_path);
    if manifest.is_empty() {
        log::error!("table-diff: no entries parsed from {manifest_path}");
        return false;
    }
    let table = crate::sweep_table::programs_full();
    let mut matched_table: Vec<bool> = vec![false; table.len()];
    let (mut ok, mut missing, mut def_mismatch, mut feat_mismatch, mut lvl_mismatch) = (0, 0, 0, 0, 0);

    for m in &manifest {
        // candidates: same ordered per-stage file lists
        let cands: Vec<usize> = table
            .iter()
            .enumerate()
            .filter(|(_, p)| {
                p.vert.iter().map(|s| s.to_string()).collect::<Vec<_>>() == m.vert
                    && p.frag.iter().map(|s| s.to_string()).collect::<Vec<_>>() == m.frag
            })
            .map(|(i, _)| i)
            .collect();
        if cands.is_empty() {
            log::warn!("MISSING   {:<40} files {:?}+{:?} defines {:?}", m.name, m.vert, m.frag, m.defines);
            missing += 1;
            continue;
        }
        // variant select: defines-exact
        let mdef: BTreeMap<String, String> = m
            .defines
            .iter()
            .filter(|(k, _)| !ignore_all.contains(k))
            .map(|(k, v)| (k.clone(), v.clone()))
            .collect();
        let def_match = |i: usize| {
            let td: BTreeMap<String, String> = table[i]
                .defines
                .iter()
                .filter(|(k, _)| !ignore_all.contains(&k.to_string()))
                .map(|(k, v)| (k.to_string(), v.clone()))
                .collect();
            td == mdef
        };
        // prefer an UNMATCHED candidate on ties (twin entries with identical files+defines,
        // e.g. benchmark_early vs benchmark, differ only in level/features)
        let exact = cands
            .iter()
            .copied()
            .find(|&i| !matched_table[i] && table[i].level == m.level && def_match(i))
            .or_else(|| cands.iter().copied().find(|&i| !matched_table[i] && def_match(i)))
            .or_else(|| cands.iter().copied().find(|&i| def_match(i)));
        let Some(ti) = exact else {
            let near = cands[0];
            let td: BTreeMap<String, String> =
                table[near].defines.iter().map(|(k, v)| (k.to_string(), v.clone())).collect();
            let only_m: Vec<_> = mdef.iter().filter(|(k, v)| td.get(*k) != Some(v)).collect();
            let only_t: Vec<_> = td.iter().filter(|(k, v)| mdef.get(*k) != Some(v)).collect();
            log::warn!(
                "DEFINES   {:<40} (nearest {}): viewer-only {:?} | table-only {:?}",
                m.name, table[near].name, only_m, only_t
            );
            def_mismatch += 1;
            continue;
        };
        matched_table[ti] = true;
        let p = &table[ti];
        let mut clean = true;
        if p.level != m.level {
            log::warn!("LEVEL     {:<40} = {}: viewer {} vs table {}", m.name, p.name, m.level, p.level);
            lvl_mismatch += 1;
            clean = false;
        }
        let tf = table_features(&p.features);
        for k in FEAT_KEYS {
            let (mv, tv) = (m.features.get(*k).copied().unwrap_or(0), tf.get(*k).copied().unwrap_or(0));
            if mv != tv {
                log::warn!("FEATURE   {:<40} = {}: {} viewer {} vs table {}", m.name, p.name, k, mv, tv);
                feat_mismatch += 1;
                clean = false;
            }
        }
        if clean {
            ok += 1;
        }
    }
    let unseen: Vec<&str> = table
        .iter()
        .enumerate()
        .filter(|(i, _)| !matched_table[*i])
        .map(|(_, p)| p.name)
        .collect();
    log::info!(
        "TABLE-DIFF: {} manifest programs vs {} table entries: {} clean matches, {} MISSING, {} define-mismatch, {} feature-mismatch, {} level-mismatch",
        manifest.len(), table.len(), ok, missing, def_mismatch, feat_mismatch, lvl_mismatch
    );
    log::info!("  NOT-SEEN (table entries the run never created; settings-gated is normal): {}", unseen.len());
    if !unseen.is_empty() {
        log::info!("    {:?}", unseen);
    }
    missing == 0 && feat_mismatch == 0 && lvl_mismatch == 0
}

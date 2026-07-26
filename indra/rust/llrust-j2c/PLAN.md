# Rust J2C Processor — Architecture & Plan (`fs/rust-j2c`)

**Status:** Planning. Branch `fs/rust-j2c`, forked from the clean LL-engine baseline
(`85e74cfc69`). No code yet — this document is the first artifact.

## Why

JPEG2000 (J2C) texture decode is the **rawest untrusted binary parse** the viewer
runs after object-update: every texture from the sim is an attacker-controllable
codestream handed to a C/C++ codec. **OpenJPEG** — the permissive fallback
decoder — has a long CVE history. We are not going to reimplement JPEG2000 (an
enormous undertaking) or simply trust the C codec. Instead we wrap it in a
**memory-safe Rust orchestration layer** that bounds every input, isolates
failures (`panic=abort` at the FFI), fails closed, and is fuzzed hard — then flip
to it as primary via the proven **shadow-then-flip** discipline (as with the
mesh/objupd Rust work). Secondary goal: **streamline fetch→decode→upload**
(off-thread decode, fewer copies) — the texture-processing optimization target.

## Ground truth (from the code)

- **Backend contract:** `LLImageJ2CImpl` (`indra/llimage/llimagej2c.h`) —
  pure-virtual `getMetadata`, `decodeImpl`, `encodeImpl`, `initDecode`,
  `initEncode`. A backend supplies a `fallbackCreateLLImageJ2CImpl()` factory.
  Selection is **compile-time**: exactly one backend `.cpp` links in.
- **Existing backends:** `llimagej2coj` (OpenJPEG, BSD-2, permissive fallback),
  `llimagej2cgrok` (Grok, AGPLv3, gated by `USE_GROK`), `llkdu` (KDU, proprietary).
- **The license split is load-bearing.** `Grok.cmake` gates Grok behind
  `USE_GROK` *because Grok is AGPLv3*, and falls back to OpenJPEG when off. So the
  Rust processor **must** support both Grok (AGPL builds) and OpenJPEG (permissive
  builds) — this is a hard requirement, not a nicety.
- SL textures are **classic JPEG2000 Part-1** codestreams: progressive
  (resolution levels + quality layers), with **discard-level** and **region**
  decode. Any backend must cover that (rules out HTJ2K-only OpenJPH).

## Codec-library evaluation

| Lib | License | Verdict |
|---|---|---|
| **Grok** | AGPLv3 | Best open-source codec, maintained, faster/safer than OJ. **AGPL-build primary.** |
| **OpenJPEG** | BSD-2 | CVE-prone (valid concern) but the **only mature *permissive* Part-1 decoder** — can't be dropped. |
| **OpenJPH** | permissive | **HTJ2K only** — does not decode SL's Part-1 textures. Out (revisit iff SL adopts HTJ2K). |
| **KDU (Kakadu)** | proprietary | Gold standard, what LL ships, not open-source-distributable. Out. |
| **Pure-Rust** | — | No production Part-1 decoder exists. The *ideal* memory-safe endgame, not viable today. |

**Decision:** keep **Grok (AGPL primary) + OpenJPEG (permissive fallback)**,
pluggable via a Rust `CodecBackend` trait that mirrors the build's license split.
OpenJPEG's bad reputation is *the argument for this branch*: since it can't be
replaced for permissive builds, we **neutralize** it with the Rust sandbox +
fuzzing. The hardening is independent of the codec; the trait keeps a future
pure-Rust decoder (the real endgame) a drop-in swap.

## Architecture

```
LLImageJ2CRust : LLImageJ2CImpl          (new C++ backend, indra/llimagej2crust/,
        │                                 provides fallbackCreateLLImageJ2CImpl)
        │  FFI  (llrust pattern: staticlib, #[no_mangle] extern "C", cbindgen)
        ▼
   llrust-j2c crate  (indra/rust/llrust-j2c/ — SEPARATE from llrust to isolate the
        │             heavy Grok/OpenJPEG C++ link)
        ├── untrusted-parse orchestration + validation (memory-safe, panic=abort)
        ├── decode -> POD {ptr,len view: pixels, w, h, components, discard_level,
        │             decode_ok} -> free   (handle-owned buffer, no per-field copy)
        └── trait CodecBackend
              ├── GrokBackend      (cargo feature "grok",     AGPL builds)
              └── OpenJpegBackend  (cargo feature "openjpeg", permissive builds)
```

Cargo features `grok`/`openjpeg` mirror the C++ `USE_GROK`/fallback toggle, so the
license configuration stays coherent end to end. Mode knob **`LL_RUSTJ2C` =
`off | cfallback | on`** (mirrors `LL_RUSTMESH`/`LL_RUSTMSG`).

## Constraints (must hold or textures silently corrupt)

1. **Deterministic decode.** JPEG2000 Part-1 decode is deterministic, so
   shadow-compare Rust-Grok vs **C++-Grok is BIT-EXACT** (`memcmp`). Rust-Grok vs
   OpenJPEG is *within-codec tolerance* only (different implementations). Compare
   against the **same codec** for the bit-exact gate.
2. **Progressive / discard-level parity.** `initDecode(discard_level, region)`
   semantics must match; the POD carries both the *requested* and the *actual*
   decoded level.
3. **Metadata parity.** `getMetadata` (dims, components, comment) must match
   exactly — it drives raw-image allocation upstream.
4. **Fail closed.** Malformed input ⇒ `decode_ok=false`, no partial/garbage
   output; the shadow excludes `!decode_ok` records from mismatch counting.
5. **Decode first, encode later.** Encode is viewer-side (snapshots/upload),
   lower risk and lower priority — keep C++ encode initially; the Rust processor
   targets **decode**.

## Phases

- **Phase 0 — scaffold + FFI handshake (no codec).** Create the `llrust-j2c`
  crate (staticlib, cbindgen, `LLRust.cmake`-style build) + an `llimagej2crust`
  C++ backend stub implementing `LLImageJ2CImpl` and FFI-returning "not handled"
  so it falls through to C++. `LL_RUSTJ2C` knob. Builds + links, does nothing —
  proves the plumbing.
- **Phase 1 — decode over Grok.** `CodecBackend` trait + `GrokBackend` (bindgen
  FFI to the grokj2k C API: init decompress → read header → set
  resolution/decode-area → decompress → image access → destroy). Implement
  `getMetadata` + `decodeImpl` (full image, discard level, region). POD + FFI
  view. `cfallback` **shadow-compare vs the C++ Grok backend → BIT-EXACT gate.**
- **Phase 2 — OpenJPEG backend + license split.** `OpenJpegBackend` (FFI to the
  OpenJPEG C API). `grok`/`openjpeg` cargo features mirror `USE_GROK`. Validate
  the permissive-build path; cross-check Grok vs OpenJPEG (tolerance).
- **Phase 3 — harden.** `cargo-fuzz` the untrusted parse: truncated streams,
  malformed markers, adversarial tile-parts, huge/again dimensions,
  integer-overflow triggers. Fail-closed, no panics/UB. **Stretch:** parse the
  codestream *structure* (markers / tile-parts / packet headers — where OJ's CVEs
  cluster) in Rust and hand only validated data to the C codec, gutting the
  riskiest surface without a full reimplementation.
- **Phase 4 — flip + streamline.** `LL_RUSTJ2C=on` primary, C++ `cfallback`.
  Then the optimization: **off-thread decode** (Rust decode is pure/thread-safe),
  decode straight into the upload-ready buffer (fewer copies), progressive /
  discard-level efficiency. A/B perf.

## Validation

- **Shadow-compare:** decoded pixels Rust-Grok vs C++-Grok **bit-exact**
  (`memcmp`) + metadata exact, across a **large real texture corpus** (texture-
  heavy regions). Zero unexplained (`decode_ok`) mismatches before any flip.
- **Fuzz:** `cargo-fuzz` corpus + crafted malformed inputs → all fail-closed.
- **Perf:** decode throughput + texture-load latency + off-thread gains,
  before/after.
- **Visual:** textures render correctly; progressive refinement + discard levels
  behave.
- **Build matrix:** AGPL build (`grok` feature) + permissive build (`openjpeg`
  feature); Windows/MSVC + FreeBSD.

## Risk register

| Risk | Mitigation |
|---|---|
| Grok vs OpenJPEG C-API differences | The `CodecBackend` trait absorbs them; the POD is codec-agnostic |
| AGPL contamination of permissive builds | Grok isolated behind a cargo feature; permissive build never links it |
| FFI buffer lifetime | handle-owned buffer + view + free (the `ll_mesh_decode` pattern); no per-field copy |
| Cross-codec nondeterminism | bit-exact shadow only vs the *same* codec; cross-codec is tolerance-only |
| Encode scope creep | decode-only first; encode stays C++ |

## Open decisions

- **Separate crate vs module in `llrust`** → lean **separate** (`llrust-j2c`), to
  isolate the heavy Grok/OpenJPEG C++ link (as wgpu is isolated in the harness).
- **bindgen vs hand-written FFI** for `grok.h` / `openjpeg.h` → **bindgen**
  (stable C APIs).
- **Shadow baseline** — bit-exact requires validating against Grok, i.e. an
  AGPL-enabled dev build; permissive validation is tolerance-only vs OpenJPEG.

## Standing principles (project-wide)

- **Mechanical validation** (shadow-compare / fuzz), never eyeballs.
- **Fail closed** on all untrusted input.
- Each phase **independently shippable**; the C++ decode path is never removed
  until the Rust path is proven (`cfallback` first, `on` last).

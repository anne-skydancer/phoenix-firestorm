//! Build script for llrust.
//!
//! The Grok bindgen now lives in the shared `j2c` crate (single source), so this build script has
//! nothing to do. grokj2k is linked into the viewer at the final link (via llimagej2cgrok), which
//! resolves the grk_* symbols the `j2c` crate references.

fn main() {}

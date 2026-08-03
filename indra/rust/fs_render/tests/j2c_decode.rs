//! End-to-end J2C decode tests. These live in fs_render (not the shared `j2c` crate) because they
//! call into Grok, and only a consumer that LINKS grokj2k (fs_render / the viewer) resolves grk_*.
//! The real-decode case is gated on FSR_TEST_J2C so the suite stays dependency-free by default.

use fs_render::j2c;

#[test]
fn decode_rejects_garbage_fail_closed() {
    j2c::ensure_init();
    assert!(j2c::decode(&[0u8; 32], 0, 0, 4).is_none());
    assert!(j2c::decode(&[], 0, 0, 4).is_none());
}

#[test]
fn decodes_a_real_j2c_when_provided() {
    let Ok(path) = std::env::var("FSR_TEST_J2C") else { return };
    let data = std::fs::read(&path).expect("read FSR_TEST_J2C");
    j2c::ensure_init();
    let img = j2c::decode(&data, 0, 0, 4).expect("decode failed");
    assert!(img.width > 0 && img.height > 0 && img.components >= 1);
    assert_eq!(img.pixels.len(), (img.width * img.height * img.components) as usize);
    eprintln!("decoded {}x{}x{}", img.width, img.height, img.components);
}

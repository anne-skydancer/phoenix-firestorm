//! UDP message-system byte decoders (memory-safe replacements for the hand-rolled
//! pointer walkers in `indra/llmessage`). These run on **unauthenticated** packets
//! straight off the socket, before any trust/identity check succeeds -- the rawest
//! attack surface the viewer has.
//!
//! First target: `zero_code_expand`, the run-length (zero-coding) decompressor.
//! The C++ original (`LLMessageSystem::zeroCodeExpand`) expands attacker-controlled
//! RLE into a fixed 8 KiB buffer guarded by three different hand-written slack
//! constants, and on a would-be overflow it resets the write pointer to the start
//! of the buffer and keeps going (recover-by-corruption). This version does every
//! write against a checked bound and *rejects* the packet instead.

/// Bytes copied verbatim from the packet head before RLE decoding begins
/// (the packet-id / flags header). C++: `LL_PACKET_ID_SIZE`.
const LL_PACKET_ID_SIZE: usize = 6;

/// High bit of the flags byte: "this packet is zero-coded". C++: `LL_ZERO_CODE_FLAG`.
const LL_ZERO_CODE_FLAG: u8 = 0x80;

/// Hard ceiling on the expanded size. C++ decodes into `mEncodedRecvBuffer`, a
/// fixed `MAX_BUFFER_SIZE` (= `NET_BUFFER_SIZE` = 0x2000) array, so a legitimate
/// packet never expands past this. C++: `MAX_BUFFER_SIZE`.
const MAX_BUFFER_SIZE: usize = 0x2000;

/// Outcome of attempting zero-code expansion.
#[derive(Debug, PartialEq, Eq)]
pub enum Expand {
    /// The zero-code flag was clear: the packet is not compressed, use it as-is.
    NotCompressed,
    /// Malformed input, or expansion would exceed `MAX_BUFFER_SIZE`. Reject the
    /// packet. (The C++ path would instead corrupt its buffer and press on.)
    Bad,
    /// Expanded byte stream (packet-id header + RLE-decoded body).
    Expanded(Vec<u8>),
}

/// Expand a possibly zero-coded received packet.
///
/// Zero-coding replaces runs of `0x00` in the body with a `0x00` marker followed
/// by count bytes: each `0x00` count byte contributes 256 zeros, and the first
/// non-zero count byte `C` terminates the run contributing `C` more -- so a run
/// `0x00` then `k` count-zeros then `C` decodes to `256*k + C` zero bytes. Every
/// other byte is literal. The `LL_PACKET_ID_SIZE`-byte header is copied verbatim
/// (with the zero-code flag cleared in byte 0, mirroring the C++).
pub fn zero_code_expand(input: &[u8]) -> Expand {
    // C++: `if (!(*data[0] & LL_ZERO_CODE_FLAG)) return 0;` -- not our packet.
    if input.is_empty() {
        return Expand::Bad;
    }
    if input[0] & LL_ZERO_CODE_FLAG == 0 {
        return Expand::NotCompressed;
    }
    // The header is copied raw; without it the packet is malformed. (The caller
    // already rejects anything shorter than LL_MINIMUM_VALID_PACKET_SIZE, but we
    // are handed untrusted bytes, so re-check.)
    if input.len() < LL_PACKET_ID_SIZE {
        return Expand::Bad;
    }

    let mut out: Vec<u8> = Vec::with_capacity(input.len().saturating_mul(2).min(MAX_BUFFER_SIZE));

    // Copy the packet-id header verbatim, then clear the zero-code flag in byte 0
    // (so downstream flag reads -- reliable/resent/ack -- see the right bits).
    out.extend_from_slice(&input[..LL_PACKET_ID_SIZE]);
    out[0] &= !LL_ZERO_CODE_FLAG;

    let mut i = LL_PACKET_ID_SIZE;
    while i < input.len() {
        let b = input[i];
        i += 1;

        if b != 0 {
            // Literal byte.
            if out.len() >= MAX_BUFFER_SIZE {
                return Expand::Bad;
            }
            out.push(b);
            continue;
        }

        // Zero run: `0x00` already consumed as the marker. Read count bytes --
        // each 0x00 adds 256, the first non-zero terminator C adds C.
        let mut run: usize = 0;
        loop {
            if i >= input.len() {
                // Ran off the end of the packet mid-run: malformed. C++ would
                // `break` and keep its partial output; we reject.
                return Expand::Bad;
            }
            let c = input[i];
            i += 1;
            if c == 0 {
                run += 256;
            } else {
                run += c as usize;
                break;
            }
        }

        // Bounds-check the whole run *before* allocating, so a crafted run (up to
        // ~256 * packet_len zeros) can never blow past the ceiling or the heap.
        if run > MAX_BUFFER_SIZE || out.len() + run > MAX_BUFFER_SIZE {
            return Expand::Bad;
        }
        out.resize(out.len() + run, 0);
    }

    Expand::Expanded(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Helper: a 6-byte header with the zero-code flag set on byte 0.
    fn hdr() -> Vec<u8> {
        vec![0x80, 0x11, 0x22, 0x33, 0x44, 0x55]
    }

    #[test]
    fn not_compressed_when_flag_clear() {
        let mut p = hdr();
        p[0] = 0x00; // flag clear
        assert_eq!(zero_code_expand(&p), Expand::NotCompressed);
    }

    #[test]
    fn header_copied_verbatim_with_flag_cleared() {
        // No body: just the header, flag set -> expands to header with flag off.
        let p = hdr();
        match zero_code_expand(&p) {
            Expand::Expanded(o) => {
                assert_eq!(o, vec![0x00, 0x11, 0x22, 0x33, 0x44, 0x55]);
            }
            other => panic!("expected Expanded, got {:?}", other),
        }
    }

    #[test]
    fn literals_pass_through() {
        let mut p = hdr();
        p.extend_from_slice(&[0x01, 0x02, 0x03]);
        match zero_code_expand(&p) {
            Expand::Expanded(o) => assert_eq!(&o[6..], &[0x01, 0x02, 0x03]),
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn simple_zero_run() {
        // 0x00 followed by count 5 -> five zero bytes.
        let mut p = hdr();
        p.extend_from_slice(&[0x00, 0x05]);
        match zero_code_expand(&p) {
            Expand::Expanded(o) => {
                assert_eq!(o.len(), 6 + 5);
                assert_eq!(&o[6..], &[0, 0, 0, 0, 0]);
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn zero_run_between_literals() {
        let mut p = hdr();
        p.extend_from_slice(&[0xAA, 0x00, 0x03, 0xBB]);
        match zero_code_expand(&p) {
            Expand::Expanded(o) => {
                assert_eq!(&o[6..], &[0xAA, 0, 0, 0, 0xBB]);
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn extended_run_256_boundary() {
        // 0x00 then one count-zero (256) then terminator 4 -> 256 + 4 = 260 zeros.
        let mut p = hdr();
        p.extend_from_slice(&[0x00, 0x00, 0x04]);
        match zero_code_expand(&p) {
            Expand::Expanded(o) => {
                assert_eq!(o.len(), 6 + 260);
                assert!(o[6..].iter().all(|&z| z == 0));
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn truncated_run_is_rejected() {
        // 0x00 marker with no following count byte: malformed.
        let mut p = hdr();
        p.push(0x00);
        assert_eq!(zero_code_expand(&p), Expand::Bad);
    }

    #[test]
    fn truncated_extended_run_is_rejected() {
        // 0x00 then a count-zero, then the packet ends: still mid-run.
        let mut p = hdr();
        p.extend_from_slice(&[0x00, 0x00]);
        assert_eq!(zero_code_expand(&p), Expand::Bad);
    }

    #[test]
    fn overflow_run_is_rejected_not_wrapped() {
        // A run that claims far more than the 8 KiB ceiling: reject, don't corrupt.
        let mut p = hdr();
        // ~40 count-zero bytes => ~40*256 ~= 10240 > 8192.
        p.push(0x00);
        for _ in 0..40 {
            p.push(0x00);
        }
        p.push(0x01); // terminator
        assert_eq!(zero_code_expand(&p), Expand::Bad);
    }

    #[test]
    fn too_short_for_header_is_rejected() {
        let p = vec![0x80, 0x11, 0x22]; // flag set but < 6 bytes
        assert_eq!(zero_code_expand(&p), Expand::Bad);
    }
}

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

// --- appended packet acknowledgements ------------------------------------
//
// A received packet may carry ACKs of previously-sent reliable packets appended
// after the (possibly zero-coded) body: `... body | id0 | id1 | ... | idN | N`,
// where each id is a big-endian TPACKETID (u32) and the trailing byte is the
// count N. The C++ path (LLMessageSystem::checkMessages) reads the count from
// the last byte, shrinks the working size by `N * sizeof(TPACKETID)`, and later
// walks the ids backwards straight out of the raw receive buffer -- signed/
// unsigned size math plus hand-indexed pointer reads. These two helpers do that
// arithmetic and those reads with checked bounds; the actual state mutation
// (LLCircuitData::ackReliablePacket) stays in C++.

/// Width of a packet id on the wire. C++: `sizeof(TPACKETID)` (TPACKETID = U32).
const TPACKETID_SIZE: i32 = 4;

/// C++: `LL_MINIMUM_VALID_PACKET_SIZE` ( = LL_PACKET_ID_SIZE + 1 ).
const LL_MINIMUM_VALID_PACKET_SIZE: i32 = 7;

/// Result of the ack size-strip (phase 1). Sizes are byte offsets into the raw
/// receive buffer, matching the C++ `receive_size` / `true_rcv_size` locals.
#[derive(Debug, PartialEq, Eq)]
pub struct AckStrip {
    /// Working size after removing the count byte AND the ack-id bytes -- i.e.
    /// the size of the message body that goes on to zeroCodeExpand.
    pub new_size: i32,
    /// Number of appended acks (the trailing count byte).
    pub acks: i32,
    /// Size after removing only the count byte -- i.e. body + ack ids. The ack
    /// ids live in `[new_size, true_rcv_size)`.
    pub true_rcv_size: i32,
}

/// Phase 1: given the current receive size and the trailing count byte, compute
/// the stripped body size. Mirrors the C++
/// `acks += buffer[--receive_size]; if (receive_size >= acks*4 + MIN) receive_size -= acks*4`.
/// Returns `None` for the malformed case the C++ rejects (`valid_packet=false`).
/// The caller guarantees `LL_ACK_FLAG` was set.
pub fn ack_strip_size(receive_size: i32, count_byte: u8) -> Option<AckStrip> {
    if receive_size < 1 {
        return None;
    }
    let acks = count_byte as i32;
    // C++ `--receive_size` consumes the trailing count byte.
    let true_rcv_size = receive_size - 1;
    // acks <= 255 so acks*4+7 <= 1027: no i32 overflow, but stay explicit.
    let need = acks
        .checked_mul(TPACKETID_SIZE)
        .and_then(|v| v.checked_add(LL_MINIMUM_VALID_PACKET_SIZE))?;
    if true_rcv_size >= need {
        Some(AckStrip {
            new_size: true_rcv_size - acks * TPACKETID_SIZE,
            acks,
            true_rcv_size,
        })
    } else {
        None
    }
}

/// Phase 2: extract the `acks` appended packet ids from the raw buffer, walking
/// backwards from `true_rcv_size` in `TPACKETID_SIZE` steps, each a big-endian
/// u32 -- mirroring the C++ `true_rcv_size -= 4; memcpy(&id, &buf[true_rcv_size], 4); ntohl(id)`
/// loop, in the same order. Returns `None` if the reads would fall outside the
/// buffer or the C++ guard `acks*4 < true_rcv_size` fails. `acks == 0` -> empty.
pub fn extract_ack_ids(buf: &[u8], true_rcv_size: i32, acks: i32) -> Option<Vec<u32>> {
    if acks < 0 || true_rcv_size < 0 {
        return None;
    }
    if acks == 0 {
        return Some(Vec::new());
    }
    let span = acks.checked_mul(TPACKETID_SIZE)?;
    // C++ guard: acks*4 < true_rcv_size (strict).
    if span >= true_rcv_size {
        return None;
    }
    // The whole ack region must lie within the received bytes.
    if (true_rcv_size as usize) > buf.len() {
        return None;
    }
    let mut ids = Vec::with_capacity(acks as usize);
    let mut pos = true_rcv_size;
    for _ in 0..acks {
        pos -= TPACKETID_SIZE;
        let p = pos as usize; // pos > 0 here: lowest is true_rcv_size - span > 0
        ids.push(u32::from_be_bytes([buf[p], buf[p + 1], buf[p + 2], buf[p + 3]]));
    }
    Some(ids)
}

// --- message-number (template) decode ------------------------------------
//
// Every received packet is routed by a message number encoded in the header
// (the bytes right after the LL_PACKET_ID_SIZE packet-id). Three frequency
// classes: high = 1 byte (< 255); medium = 0xFF then 1 byte; low = 0xFF 0xFF
// then a big-endian u16. Mirrors LLTemplateMessageReader::decodeTemplate's
// number decode -- the classification/read, not the template-map lookup.

/// Resolve the message number from the first `buffer_size` bytes of a packet
/// (`buf`). Returns `None` if the packet is too short to classify -- including
/// the case C++ leaves unguarded (a body shorter than one header byte, which it
/// reads past; here it is rejected). The map lookup stays in C++.
pub fn decode_template_number(buf: &[u8]) -> Option<u32> {
    // Need the packet id plus at least one header byte (header[0]).
    if buf.len() < LL_PACKET_ID_SIZE + 1 {
        return None;
    }
    let header = &buf[LL_PACKET_ID_SIZE..];
    let buffer_size = buf.len();

    if header[0] != 255 {
        // high frequency
        Some(header[0] as u32)
    } else if buffer_size >= (LL_MINIMUM_VALID_PACKET_SIZE as usize + 1) && header[1] != 255 {
        // medium frequency
        Some((255u32 << 8) | header[1] as u32)
    } else if buffer_size >= (LL_MINIMUM_VALID_PACKET_SIZE as usize + 3) && header[1] == 255 {
        // low frequency: header[2..4] big-endian u16
        Some(0xFFFF_0000u32 | u16::from_be_bytes([header[2], header[3]]) as u32)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Helper: a 6-byte header with the zero-code flag set on byte 0.
    fn hdr() -> Vec<u8> {
        vec![0x80, 0x11, 0x22, 0x33, 0x44, 0x55]
    }

    // Helper: 6 packet-id bytes then the given header bytes.
    fn pkt(header: &[u8]) -> Vec<u8> {
        let mut v = vec![0u8; LL_PACKET_ID_SIZE];
        v.extend_from_slice(header);
        v
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

    // --- ack strip (phase 1) ---

    #[test]
    fn ack_strip_two_acks() {
        // receive_size = 20, count byte = 2 -> strip 1 count + 2*4 = 9 bytes.
        let s = ack_strip_size(20, 2).unwrap();
        assert_eq!(s.acks, 2);
        assert_eq!(s.true_rcv_size, 19); // after count byte
        assert_eq!(s.new_size, 19 - 8); // minus 2*4 ack ids
    }

    #[test]
    fn ack_strip_zero_acks() {
        // count byte 0: nothing stripped beyond the count byte itself.
        let s = ack_strip_size(20, 0).unwrap();
        assert_eq!(s.acks, 0);
        assert_eq!(s.true_rcv_size, 19);
        assert_eq!(s.new_size, 19);
    }

    #[test]
    fn ack_strip_rejects_when_too_many_acks() {
        // 10 acks claim 40 bytes but only ~19 remain -> malformed.
        assert_eq!(ack_strip_size(20, 10), None);
    }

    #[test]
    fn ack_strip_rejects_at_min_boundary() {
        // true_rcv_size must be >= acks*4 + 7. With 1 ack: need >= 11.
        assert!(ack_strip_size(12, 1).is_some()); // true=11, need=11 -> ok
        assert_eq!(ack_strip_size(11, 1), None); // true=10, need=11 -> reject
    }

    // --- ack id extraction (phase 2) ---

    #[test]
    fn extract_two_ids_big_endian_backwards() {
        // body(4) | id0=0x00000001 | id1=0x02030405 ; true_rcv_size = 12
        let buf: Vec<u8> = vec![
            0xAA, 0xBB, 0xCC, 0xDD, // body
            0x00, 0x00, 0x00, 0x01, // id at offset 4
            0x02, 0x03, 0x04, 0x05, // id at offset 8
        ];
        // C++ reads backwards: first the one at offset 8, then offset 4.
        let ids = extract_ack_ids(&buf, 12, 2).unwrap();
        assert_eq!(ids, vec![0x02030405, 0x00000001]);
    }

    #[test]
    fn extract_zero_acks_is_empty() {
        let buf = vec![0u8; 8];
        assert_eq!(extract_ack_ids(&buf, 8, 0), Some(vec![]));
    }

    #[test]
    fn extract_rejects_when_span_exceeds_true_size() {
        // 3 acks -> 12 bytes, but true_rcv_size = 12 (not strictly greater) -> reject.
        let buf = vec![0u8; 16];
        assert_eq!(extract_ack_ids(&buf, 12, 3), None);
    }

    #[test]
    fn extract_rejects_when_true_size_past_buffer() {
        let buf = vec![0u8; 8];
        assert_eq!(extract_ack_ids(&buf, 12, 1), None); // true_rcv_size > buf.len()
    }

    // --- template (message number) decode ---

    #[test]
    fn template_high_frequency() {
        // header[0] < 255 -> the number itself.
        let p = pkt(&[0x07, 0x00]);
        assert_eq!(decode_template_number(&p), Some(7));
    }

    #[test]
    fn template_medium_frequency() {
        // header = 0xFF, 0x0A ; needs buffer_size >= 8.
        let p = pkt(&[0xFF, 0x0A]); // total len = 8
        assert_eq!(decode_template_number(&p), Some((255 << 8) | 0x0A));
    }

    #[test]
    fn template_low_frequency() {
        // header = 0xFF, 0xFF, then big-endian u16 0x002A ; needs len >= 10.
        let p = pkt(&[0xFF, 0xFF, 0x00, 0x2A]); // total len = 10
        assert_eq!(decode_template_number(&p), Some(0xFFFF_0000 | 0x002A));
    }

    #[test]
    fn template_low_frequency_high_u16() {
        let p = pkt(&[0xFF, 0xFF, 0x12, 0x34]);
        assert_eq!(decode_template_number(&p), Some(0xFFFF_1234));
    }

    #[test]
    fn template_too_short_for_header() {
        let p = vec![0u8; LL_PACKET_ID_SIZE]; // no header byte at all
        assert_eq!(decode_template_number(&p), None);
    }

    #[test]
    fn template_medium_marker_but_too_short_is_none() {
        // header[0]=0xFF but only 7 bytes total: cannot be medium (needs 8) or
        // low (needs 10), and header[1] must not be read.
        let p = pkt(&[0xFF]); // total len = 7
        assert_eq!(decode_template_number(&p), None);
    }

    #[test]
    fn template_low_marker_but_too_short_is_none() {
        // 0xFF 0xFF but only 8 bytes: low needs 10 -> None (no OOB read).
        let p = pkt(&[0xFF, 0xFF]); // total len = 8
        assert_eq!(decode_template_number(&p), None);
    }
}

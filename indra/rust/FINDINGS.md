# Firestorm hardening findings

A running punch-list surfaced as a byproduct of deep-reading subsystems for the
Rust migration. Two kinds: **latent bugs** (real defects) and **dead code**.
Each entry notes location, the defect, a repro sketch, and a **disposition**:

- `PRESERVED` — reproduced bit-for-bit in a refactor/port to keep behavior
  identical; a fix is deferred so it doesn't muddy shadow validation.
- `HARDENED` — the Rust path already rejects/handles it; C++ fallback may still exhibit it.
- `OPEN` — noted, no action yet.
- `FIXED` — corrected in-tree (reference the commit).

The north star this feeds: understand + optimize the render subsystem well
enough to theorize an OpenGL → Vulkan migration (see the message-system-rewrite
vision). Dead-code removal and bug triage are how we earn that understanding.

---

## Latent bugs

### 1. `zeroCodeExpand` — reset-and-continue overflow "recovery" · HARDENED
`indra/llmessage/message.cpp` (`LLMessageSystem::zeroCodeExpand`). On a
would-be overrun of the fixed `mEncodedRecvBuffer`, the C++ path resets
`outptr` to the buffer start and keeps decoding — corrupting the buffer rather
than rejecting the packet. **Repro:** a zero-coded packet whose RLE expands past
`MAX_BUFFER_SIZE`. **Disposition:** the Rust decoder (`msg.rs::zero_code_expand`)
returns `Bad` (reject); C++ remains the `cfallback`. Committed `747c01f53c` /
`e39d6cfc0c`.

### 2. Object-update scratch-data heap overflow · PRESERVED
`indra/newview/llviewerobject.cpp`, compressed `OUT_FULL` path (0x1 flag).
`mData = new U8[ScratchPadSize]` then `unpackBinaryData(mData, sp_size)` copies
`sp_size` bytes — where `sp_size` is the payload's *own* length prefix,
independent of `ScratchPadSize`. If `sp_size > ScratchPadSize`, heap overflow.
**Repro:** a compressed update with tree/scratch data whose inner length prefix
exceeds the advertised ScratchPadSize. **Disposition:** reproduced bit-for-bit
in `applyCompressedUpdate` (commit `8a7de89d57`) so the Phase-2 shadow stays
honest; candidate real fix once the Rust path is authoritative.

### 3. `decodeColorMap` RLE loop — no bounds guard · OPEN
`indra/llimage/llimagetga.cpp`. The truecolor RLE paths check `last_src`, but the
color-map RLE loop walks `src` with no end guard — a classic over-read on a
crafted `.tga`. **Disposition:** target of the TGA Rust port (bounds-checked
slices). OPEN.

### 4. `countTrailingZeros` zero-mask infinite loop · OPEN
`indra/llimage/llimagebmp.cpp` (`decodeColorMask16`). A zero bitfield mask makes
the trailing-zero count loop spin forever; the 16-bit BMP path also lacks the
size guard the 24/32-bit paths have. **Disposition:** target of the BMP Rust
port. OPEN.

### 5. `unpackString` strlen-before-verify · OPEN
`indra/llmessage/lldatapacker.cpp` (`LLDataPackerBinaryBuffer::unpackString`).
Runs `strlen` on the raw buffer *before* `verifyLength`, over-reading if the blob
isn't NUL-terminated within bounds. **Disposition:** OPEN; hardened where the
Rust message decoders replace the call site.

### 6. Ignored `verifyLength` returns (systemic) · OPEN
`processUpdateMessage` and peers call `unpack*` and discard every `bool` return.
`verifyLength` fails *before* advancing (so it's bounded), but the code then
proceeds on stale/garbage values instead of aborting. **Disposition:** the Rust
decoders track a `decode_ok` flag reflecting these; C++ behavior preserved for now.

---

## Dead code

_(none logged yet — fill as the render-subsystem recon surfaces them)_

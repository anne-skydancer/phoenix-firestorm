/**
 * @file llobjectupdatepod.h
 * @brief Plain decoded object-update data (the seam between decode and apply).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
 * $/LicenseInfo$
 */

#ifndef LL_LLOBJECTUPDATEPOD_H
#define LL_LLOBJECTUPDATEPOD_H

#include "stdtypes.h"
#include "v3math.h"
#include "v4math.h"
#include "llquaternion.h"
#include "lluuid.h"
#include <string>
#include <vector>

// Plain-old-data result of decoding one object update off the wire.
//
// It is filled by the pure LLViewerObject::decode* functions (which touch NO
// object state) and consumed by the apply step, which does all the eager
// set*/allocation/side-effect mutation. One struct covers all three update
// paths (full / terse / compressed); the presence bools and `update_type` say
// which fields are live. See the plan (atomic-yawning-badger.md) and the
// constraints in LLViewerObject::processUpdateMessage.
//
// Design rules that keep the Rust shadow (Phase 2) bit-exact:
//  - Decode does NO object mutation and NO quaternion reconstruction: it carries
//    the raw rotation vector for the full/compressed paths (apply does
//    unpackFromVector3 + normQuat) and the already-dequantized quaternion for
//    the terse path. Text-color alpha inversion (255-a) also happens in apply.
//  - Variable-length fields are owned here (std::vector / std::string). In the
//    Rust phase the same values are exposed as ptr+len views into a handle-owned
//    buffer; the shadow compares values, not memory layout.
struct LLObjectUpdatePod
{
    // EObjectUpdateType value, stored numerically to keep this header free of a
    // circular include on llviewerobject.h:
    //   0 OUT_FULL   1 OUT_TERSE_IMPROVED   2 OUT_FULL_COMPRESSED
    //   3 OUT_FULL_CACHED   4 OUT_UNKNOWN
    S32         update_type = 4;      // default OUT_UNKNOWN
    bool        from_compressed = false;  // dp-sourced (compressed/cached) vs message
    bool        decode_ok = true;     // false if any read would have overrun the buffer

    // --- attachment state ---
    bool        has_state = false;
    U8          state = 0;

    // --- motion (fully dequantized into region/world units by decode) ---
    bool        has_pos = false;      LLVector3   pos;
    bool        has_scale = false;    LLVector3   scale;
    // Rotation: exactly one representation is live.
    //   rot_from_vec3 == true  -> apply does new_rot.unpackFromVector3(rot_vec)  (full/compressed)
    //   rot_from_vec3 == false -> apply uses rot_quat directly                    (terse)
    bool        has_rot = false;
    bool        rot_from_vec3 = false;
    LLVector3   rot_vec;
    LLQuaternion rot_quat;
    bool        has_vel = false;      LLVector3   vel;
    bool        has_accel = false;    LLVector3   accel;   // compressed-full: present and zero
    bool        has_angv = false;     LLVector3   angv;    // omega
    bool        has_collision_plane = false;  LLVector4 collision_plane;
    S32         precision = 32;        // this_update_precision, in bits

    // --- compressed-full scalar block ---
    bool        has_crc = false;      U32 crc = 0;
    bool        has_material = false; U8  material = 0;
    bool        has_click_action = false; U8 click_action = 0;
    bool        has_owner_id = false; LLUUID owner_id;
    bool        has_parent = false;   U32 parent_id = 0;    // 0 when absent (see apply)

    // --- generic data (compressed): 0 none, 1 tree (1 byte), 2 scratch ---
    U8          gen_kind = 0;
    U8          tree_byte = 0;
    // The scratch path carries BOTH the ScratchPadSize allocation size and the
    // separately length-prefixed blob, on purpose: the C++ path allocates
    // new U8[scratch_alloc_size] but copies the blob's own length. Preserving
    // both keeps behavior (and the latent-overflow bug) bit-identical; do not
    // "fix" it here.
    U32         scratch_alloc_size = 0;
    std::vector<U8> scratch_blob;

    // --- hover text ---
    bool        has_text = false;
    std::string text_utf8;
    U8          text_color[4] = { 0, 0, 0, 0 };  // RAW; apply inverts alpha (255-a)

    // --- media URL (compressed: presence-gated; full: always applied, "" clears) ---
    bool        has_media_url = false;
    std::string media_url;

    // --- particles (legacy fixed-size block, compressed 0x8) ---
    bool        has_legacy_particles = false;
    std::vector<U8> particle_legacy_blob;

    // --- extra-params TLV list (wire order preserved) ---
    struct ExtraParam { U16 type = 0; std::vector<U8> data; };
    bool        has_extra_params = false;   // full: ExtraParams field present; compressed: always reads count
    std::vector<ExtraParam> params;

    // --- attached sound ---
    bool        has_sound = false;
    LLUUID      sound_uuid;
    LLUUID      sound_owner_id;    // full path: separate OwnerID field; compressed: == owner_id
    F32         sound_gain = 0.f;
    U8          sound_flags = 0;
    F32         sound_cutoff = 0.f;

    // --- name-value list ---
    bool        has_name_value = false;
    std::string name_value;

    // --- full-message-only extras ---
    bool        has_update_flags = false; U32 update_flags = 0;   // _PREHASH_UpdateFlags
    bool        create_selected = false;
    // OUT_FULL generic data has different semantics (tree genome vs prim-count
    // sample) than the compressed scratch/tree path; kept separate on purpose.
    bool        has_full_generic = false;
    std::vector<U8> full_generic_data;

    // --- decoder bookkeeping (compressed path; NOT wire data) ---
    // Bytes the compressed decode consumed from the LLDataPacker. At the Phase-3
    // flip the caller must dp->shift(final_cursor_offset) + setPassFlags(pass_flags)
    // so the LLVOVolume / LLVOAvatar overrides resume reading at the right spot.
    U32         final_cursor_offset = 0;
    U32         pass_flags = 0;     // == the compressed SpecialCode word
};

#endif // LL_LLOBJECTUPDATEPOD_H

#!/usr/bin/env python3
"""Inspect and validate an R5b LLGHIM5B material/skin packet."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path


class Reader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def take(self, size: int) -> bytes:
        end = self.offset + size
        if end > len(self.data):
            raise ValueError("truncated packet")
        value = self.data[self.offset:end]
        self.offset = end
        return value

    def u32(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def u64(self) -> int:
        return struct.unpack("<Q", self.take(8))[0]

    def skip_floats(self, count: int) -> None:
        self.take(count * 4)


def inspect(path: Path) -> dict[str, object]:
    data = path.read_bytes()
    reader = Reader(data)
    if reader.take(8) != b"LLGHIM5B":
        raise ValueError("not an LLGHIM5B packet")
    version = reader.u32()
    reader.u32()
    frame, scene_epoch, resource_epoch = reader.u64(), reader.u64(), reader.u64()
    texture_count, material_count, skin_count, draw_count = (
        reader.u64(), reader.u64(), reader.u64(), reader.u64()
    )
    reasons: dict[int, int] = {}
    payload_bytes = 0
    content_hashes_verified = 0
    for _ in range(texture_count):
        reader.take(32)
        content_hash = reader.take(32).hex()
        reader.u32()
        comparability = reader.u32()
        reasons[comparability] = reasons.get(comparability, 0) + 1
        width, height, components, _discard = (
            reader.u32(), reader.u32(), reader.u32(), reader.u32()
        )
        size = reader.u64()
        pixels = reader.take(size)
        payload_bytes += size
        if size:
            if size != width * height * components:
                raise ValueError("decoded texture byte count does not match dimensions")
            if hashlib.sha256(pixels).hexdigest() != content_hash:
                raise ValueError("decoded texture content hash mismatch")
            content_hashes_verified += 1
        elif int(content_hash, 16):
            raise ValueError("metadata-only texture has a nonzero content identity")
    for _ in range(material_count):
        reader.take(32)
        reader.take(6 * 4)
        reader.skip_floats(4 + 3 + 4 + 4)
        binding_count = reader.u64()
        for _ in range(binding_count):
            reader.take(4 * 4)
            reader.skip_floats(5)
    for _ in range(skin_count):
        reader.take(32)
        comparability, joints = reader.u32(), reader.u32()
        reasons[comparability] = reasons.get(comparability, 0) + 1
        reader.skip_floats(joints * 12)
    comparable_draws = 0
    for _ in range(draw_count):
        reader.u64()
        reader.u32()
        reader.u32()
        comparability = reader.u32()
        reader.u32()
        reasons[comparability] = reasons.get(comparability, 0) + 1
        comparable_draws += comparability == 0
    if reader.offset != len(data):
        raise ValueError("packet has trailing data")
    return {
        "path": str(path.resolve()),
        "sha256": hashlib.sha256(data).hexdigest(),
        "version": version,
        "frame": frame,
        "scene_epoch": scene_epoch,
        "resource_epoch": resource_epoch,
        "textures": texture_count,
        "textures_with_verified_content": content_hashes_verified,
        "decoded_texture_bytes": payload_bytes,
        "materials": material_count,
        "skins": skin_count,
        "draws": draw_count,
        "comparable_draws": comparable_draws,
        "comparability_masks": {str(key): value for key, value in sorted(reasons.items())},
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("packet", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(inspect(args.packet), indent=2))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

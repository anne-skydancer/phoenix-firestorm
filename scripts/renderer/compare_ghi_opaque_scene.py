#!/usr/bin/env python3
"""Replay one R4d scene packet on both native peers and enforce bounded parity."""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import subprocess
import sys
import tempfile


BYTES_PER_PIXEL = (4, 4, 8, 8)


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command, text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    print(completed.stdout, end="")
    if completed.returncode:
        raise RuntimeError(
            f"peer replay exited with {completed.returncode}: {' '.join(command)}")
    return completed.stdout


def token(output: str, name: str) -> str:
    prefix = name + "="
    for item in output.split():
        if item.startswith(prefix):
            return item[len(prefix):]
    raise RuntimeError(f"peer output omitted {name}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packet", type=pathlib.Path, required=True)
    parser.add_argument("--opengl", type=pathlib.Path, required=True)
    parser.add_argument("--vulkan", type=pathlib.Path, required=True)
    parser.add_argument("--max-differing-pixels", type=int, default=32)
    parser.add_argument("--max-coverage-delta", type=int, default=8)
    parser.add_argument("--no-validation", action="store_true")
    args = parser.parse_args()

    for path in (args.packet, args.opengl, args.vulkan):
        if not path.is_file():
            parser.error(f"file not found: {path}")
    packet_hash = hashlib.sha256(args.packet.read_bytes()).hexdigest()

    with tempfile.TemporaryDirectory(prefix="ghi_r4d_compare_") as temporary:
        root = pathlib.Path(temporary)
        gl_prefix = root / "gl"
        vk_prefix = root / "vk"
        gl_output = run([
            str(args.opengl), "--packet", str(args.packet),
            "--dump-prefix", str(gl_prefix),
        ])
        vk_command = [str(args.vulkan)]
        if not args.no_validation:
            vk_command.append("--validation")
        vk_command += [
            "--packet", str(args.packet), "--dump-prefix", str(vk_prefix)]
        vk_output = run(vk_command)

        if token(gl_output, "packet-sha256") != packet_hash or \
                token(vk_output, "packet-sha256") != packet_hash:
            raise RuntimeError("peers did not report the input packet identity")
        for name in ("draws", "triangles", "submitted-draws", "submitted-triangles",
                     "source", "frame", "production-occlusion", "skipped-rigged",
                     "skipped-material", "invalid"):
            if token(gl_output, name) != token(vk_output, name):
                raise RuntimeError(f"peer structural evidence differs for {name}")

        failed = False
        for target, bpp in enumerate(BYTES_PER_PIXEL):
            gl = (root / f"gl-target{target}.bin").read_bytes()
            vk = (root / f"vk-target{target}.bin").read_bytes()
            if len(gl) != len(vk) or len(gl) % bpp:
                raise RuntimeError(f"target {target} readback sizes are incompatible")
            differing = sum(
                gl[offset:offset + bpp] != vk[offset:offset + bpp]
                for offset in range(0, len(gl), bpp))
            gl_coverage = sum(
                any(gl[offset:offset + bpp]) for offset in range(0, len(gl), bpp))
            vk_coverage = sum(
                any(vk[offset:offset + bpp]) for offset in range(0, len(vk), bpp))
            coverage_delta = abs(gl_coverage - vk_coverage)
            print(
                f"target{target}: differing_pixels={differing} "
                f"coverage_gl={gl_coverage} coverage_vk={vk_coverage} "
                f"coverage_delta={coverage_delta}")
            if (differing > args.max_differing_pixels or
                    coverage_delta > args.max_coverage_delta):
                failed = True

    if failed:
        print("R4d live opaque peer comparison FAIL", file=sys.stderr)
        return 1
    print(
        "R4d live opaque peer comparison PASS "
        f"packet-sha256={packet_hash} max-differing-pixels="
        f"{args.max_differing_pixels} max-coverage-delta={args.max_coverage_delta}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"compare_ghi_opaque_scene: {error}", file=sys.stderr)
        sys.exit(2)

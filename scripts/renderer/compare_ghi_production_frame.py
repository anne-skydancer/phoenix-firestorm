#!/usr/bin/env python3
"""Replay one real production frame on both GHI peers and compare semantics."""

from __future__ import annotations

import argparse
import hashlib
import math
import pathlib
import subprocess
import sys


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


def integers(output: str, name: str) -> list[int]:
    try:
        return [int(value) for value in token(output, name).split(",")]
    except ValueError as error:
        raise RuntimeError(f"peer output has invalid {name}") from error


def compare_coverage(
        name: str, opengl: list[int], vulkan: list[int],
        absolute_tolerance: int, relative_tolerance: float) -> bool:
    if len(opengl) != len(vulkan):
        raise RuntimeError(f"peer {name} lengths differ")
    failed = False
    for index, (gl_value, vk_value) in enumerate(zip(opengl, vulkan)):
        delta = abs(gl_value - vk_value)
        allowed = max(
            absolute_tolerance,
            math.ceil(max(gl_value, vk_value) * relative_tolerance),
        )
        print(
            f"{name}{index}: opengl={gl_value} vulkan={vk_value} "
            f"delta={delta} allowed={allowed}")
        failed |= delta > allowed
    return not failed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packet", type=pathlib.Path, required=True)
    parser.add_argument("--opengl", type=pathlib.Path, required=True)
    parser.add_argument("--vulkan", type=pathlib.Path, required=True)
    parser.add_argument("--adapter", type=int, default=0)
    parser.add_argument("--absolute-coverage-tolerance", type=int, default=256)
    parser.add_argument("--relative-coverage-tolerance", type=float, default=0.001)
    parser.add_argument("--no-validation", action="store_true")
    args = parser.parse_args()

    for path in (args.packet, args.opengl, args.vulkan):
        if not path.is_file():
            parser.error(f"file not found: {path}")
    if args.adapter < 0:
        parser.error("--adapter must not be negative")
    if args.absolute_coverage_tolerance < 0:
        parser.error("--absolute-coverage-tolerance must not be negative")
    if not 0.0 <= args.relative_coverage_tolerance <= 1.0:
        parser.error("--relative-coverage-tolerance must be between 0 and 1")

    packet_hash = hashlib.sha256(args.packet.read_bytes()).hexdigest()
    common = [str(args.packet), "--adapter", str(args.adapter)]
    gl_output = run([str(args.opengl), *common])
    vk_command = [str(args.vulkan), *common]
    if not args.no_validation:
        vk_command.append("--validation")
    vk_output = run(vk_command)

    if token(gl_output, "backend") != "OpenGL":
        raise RuntimeError("OpenGL replay identified the wrong backend")
    if token(vk_output, "backend") != "Vulkan":
        raise RuntimeError("Vulkan replay identified the wrong backend")
    for output in (gl_output, vk_output):
        if token(output, "packet-sha256") != packet_hash:
            raise RuntimeError("peer did not report the input packet identity")

    for name in (
            "frame", "assembly-epoch", "extent", "draws", "residency",
            "lights", "shadow-draws", "shadow-active"):
        if token(gl_output, name) != token(vk_output, name):
            raise RuntimeError(f"peer structural evidence differs for {name}")

    gl_gbuffer = integers(gl_output, "gbuffer-coverage")
    vk_gbuffer = integers(vk_output, "gbuffer-coverage")
    gl_lighting = integers(gl_output, "lighting-coverage")
    vk_lighting = integers(vk_output, "lighting-coverage")
    gl_shadows = integers(gl_output, "shadow-coverage")
    vk_shadows = integers(vk_output, "shadow-coverage")
    shadow_active = integers(gl_output, "shadow-active")
    lights = integers(gl_output, "lights")

    if len(gl_gbuffer) != 4 or not all(gl_gbuffer[:3]) or not all(vk_gbuffer[:3]):
        raise RuntimeError("a required geometry G-buffer target contains no production work")
    if len(gl_lighting) != 1 or len(vk_lighting) != 1 or \
            not gl_lighting[0] or not vk_lighting[0]:
        raise RuntimeError("the production lighting target contains no work")
    if len(shadow_active) != len(gl_shadows) or len(gl_shadows) != len(vk_shadows):
        raise RuntimeError("shadow evidence has inconsistent target counts")
    if len(lights) != 7:
        raise RuntimeError("lighting evidence has an unexpected shape")
    directional_maps, projector_maps = lights[5], lights[6]
    directional_slots = shadow_active[:4]
    projector_slots = shadow_active[4:]
    if directional_maps and not any(
            active and gl_shadows[index] and vk_shadows[index]
            for index, active in enumerate(directional_slots)):
        raise RuntimeError("directional shadows have no comparable covered map")
    if projector_maps and not any(
            active and gl_shadows[index + 4] and vk_shadows[index + 4]
            for index, active in enumerate(projector_slots)):
        raise RuntimeError("projector shadows have no comparable covered map")

    passed = compare_coverage(
        "gbuffer", gl_gbuffer, vk_gbuffer,
        args.absolute_coverage_tolerance, args.relative_coverage_tolerance)
    passed &= compare_coverage(
        "lighting", gl_lighting, vk_lighting,
        args.absolute_coverage_tolerance, args.relative_coverage_tolerance)
    passed &= compare_coverage(
        "shadow", gl_shadows, vk_shadows,
        args.absolute_coverage_tolerance, args.relative_coverage_tolerance)
    if not passed:
        print("P0e1 production-frame peer comparison FAIL", file=sys.stderr)
        return 1

    print(
        "P0e1 production-frame peer comparison PASS "
        f"packet-sha256={packet_hash} "
        f"absolute-coverage-tolerance={args.absolute_coverage_tolerance} "
        f"relative-coverage-tolerance={args.relative_coverage_tolerance}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"compare_ghi_production_frame: {error}", file=sys.stderr)
        sys.exit(2)

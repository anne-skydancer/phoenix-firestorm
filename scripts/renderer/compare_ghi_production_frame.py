#!/usr/bin/env python3
"""Replay one real production frame on both GHI peers and compare semantics."""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


def run(command: list[str], environment: dict[str, str] | None = None) -> str:
    completed = subprocess.run(
        command, text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
        env=environment,
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


def read_visible_reference(path: pathlib.Path) -> tuple[int, int, int, bytes, bytes]:
    data = path.read_bytes()
    if len(data) < 24 or data[:4] != b"GHIV":
        raise RuntimeError("visible reference has an invalid header")
    version, width, height, frame = struct.unpack_from("<IIIQ", data, 4)
    size = width * height * 3
    if version != 1 or not width or not height or len(data) != 24 + size * 2:
        raise RuntimeError("visible reference has an invalid payload")
    return width, height, frame, data[24:24 + size], data[24 + size:]


def read_water_output(path: pathlib.Path) -> tuple[int, int, int, bytes, bytes]:
    data = path.read_bytes()
    if len(data) < 28 or data[:4] != b"GHIW":
        raise RuntimeError("peer water output has an invalid header")
    version, width, height, pixel_bytes, frame = struct.unpack_from(
        "<IIIIQ", data, 4)
    size = width * height * pixel_bytes
    if version != 1 or not width or not height or pixel_bytes != 8 or \
            len(data) != 28 + size * 2:
        raise RuntimeError("peer water output has an invalid payload")
    return width, height, frame, data[28:28 + size], data[28 + size:]


def compare_visible_water(
        reference_path: pathlib.Path, peer_path: pathlib.Path,
        minimum_mask_iou: float, maximum_delta_mae: float) -> bool:
    source_width, source_height, reference_frame, visible_before, visible_after = \
        read_visible_reference(reference_path)
    width, height, peer_frame, peer_before, peer_after = read_water_output(peer_path)
    if reference_frame != peer_frame:
        raise RuntimeError("visible reference and peer water output frames differ")
    if source_width % width or source_height % height:
        raise RuntimeError("visible reference cannot be evenly reduced to peer extent")
    scale_x = source_width // width
    scale_y = source_height // height
    sample_count = scale_x * scale_y
    visible_changed = 0
    peer_changed = 0
    intersection = 0
    union = 0
    error = 0.0
    for y in range(height):
        for x in range(width):
            visible_delta = [0.0, 0.0, 0.0]
            for canonical_y in range(y * scale_y, (y + 1) * scale_y):
                source_y = source_height - 1 - canonical_y
                offset = (source_y * source_width + x * scale_x) * 3
                for source_x in range(scale_x):
                    pixel = offset + source_x * 3
                    for component in range(3):
                        visible_delta[component] += (
                            visible_after[pixel + component] -
                            visible_before[pixel + component]) / 255.0
            visible_delta = [value / sample_count for value in visible_delta]
            peer_pixel = (y * width + x) * 8
            before = struct.unpack_from("<4e", peer_before, peer_pixel)
            after = struct.unpack_from("<4e", peer_after, peer_pixel)
            peer_delta = [
                max(0.0, min(1.0, after[component])) -
                max(0.0, min(1.0, before[component]))
                for component in range(3)]
            visible_mask = max(map(abs, visible_delta)) >= 1.0 / 255.0
            peer_mask = max(map(abs, peer_delta)) >= 1.0 / 1024.0
            visible_changed += visible_mask
            peer_changed += peer_mask
            intersection += visible_mask and peer_mask
            union += visible_mask or peer_mask
            if visible_mask or peer_mask:
                error += sum(abs(a - b) for a, b in zip(
                    visible_delta, peer_delta))
    if not visible_changed or not peer_changed or not union:
        raise RuntimeError("visible-reference comparison has an empty water mask")
    mask_iou = intersection / union
    delta_mae = error / (union * 3)
    print(
        "visible-water-reference: "
        f"frame={peer_frame} visible-modified={visible_changed} "
        f"peer-modified={peer_changed} mask-iou={mask_iou:.6f} "
        f"delta-mae={delta_mae:.6f} minimum-mask-iou={minimum_mask_iou} "
        f"maximum-delta-mae={maximum_delta_mae}")
    return mask_iou >= minimum_mask_iou and delta_mae <= maximum_delta_mae


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packet", type=pathlib.Path, required=True)
    parser.add_argument("--environment", type=pathlib.Path)
    parser.add_argument("--opengl", type=pathlib.Path, required=True)
    parser.add_argument("--vulkan", type=pathlib.Path, required=True)
    parser.add_argument("--adapter", type=int, default=0)
    parser.add_argument("--absolute-coverage-tolerance", type=int, default=256)
    parser.add_argument("--relative-coverage-tolerance", type=float, default=0.001)
    parser.add_argument("--visible-reference", type=pathlib.Path)
    parser.add_argument("--minimum-water-mask-iou", type=float, default=0.5)
    parser.add_argument("--maximum-water-delta-mae", type=float, default=0.2)
    parser.add_argument("--no-validation", action="store_true")
    args = parser.parse_args()

    paths = [args.packet, args.opengl, args.vulkan]
    if args.environment is not None:
        paths.append(args.environment)
    for path in paths:
        if not path.is_file():
            parser.error(f"file not found: {path}")
    if args.adapter < 0:
        parser.error("--adapter must not be negative")
    if args.absolute_coverage_tolerance < 0:
        parser.error("--absolute-coverage-tolerance must not be negative")
    if not 0.0 <= args.relative_coverage_tolerance <= 1.0:
        parser.error("--relative-coverage-tolerance must be between 0 and 1")
    if not 0.0 <= args.minimum_water_mask_iou <= 1.0:
        parser.error("--minimum-water-mask-iou must be between 0 and 1")
    if not 0.0 <= args.maximum_water_delta_mae <= 1.0:
        parser.error("--maximum-water-delta-mae must be between 0 and 1")
    if args.visible_reference is not None and args.environment is None:
        parser.error("--visible-reference requires --environment")
    if args.visible_reference is not None and not args.visible_reference.is_file():
        parser.error(f"file not found: {args.visible_reference}")

    packet_hash = hashlib.sha256(args.packet.read_bytes()).hexdigest()
    common = [str(args.packet), "--adapter", str(args.adapter)]
    environment_hash = None
    if args.environment is not None:
        environment_hash = hashlib.sha256(args.environment.read_bytes()).hexdigest()
        common.extend(["--environment", str(args.environment)])
    temporary = tempfile.TemporaryDirectory(prefix="ghi-water-")
    temporary_path = pathlib.Path(temporary.name)
    gl_water = temporary_path / "opengl.ghiw"
    vk_water = temporary_path / "vulkan.ghiw"
    gl_environment = os.environ.copy()
    gl_environment["VULKANSTORM_GHI_WATER_OUTPUT"] = str(gl_water)
    vk_environment = os.environ.copy()
    vk_environment["VULKANSTORM_GHI_WATER_OUTPUT"] = str(vk_water)
    gl_output = run([str(args.opengl), *common], gl_environment)
    vk_command = [str(args.vulkan), *common]
    if not args.no_validation:
        vk_command.append("--validation")
    vk_output = run(vk_command, vk_environment)

    if token(gl_output, "backend") != "OpenGL":
        raise RuntimeError("OpenGL replay identified the wrong backend")
    if token(vk_output, "backend") != "Vulkan":
        raise RuntimeError("Vulkan replay identified the wrong backend")
    for output in (gl_output, vk_output):
        if token(output, "packet-sha256") != packet_hash:
            raise RuntimeError("peer did not report the input packet identity")
        if environment_hash is not None and \
                token(output, "environment-sha256") != environment_hash:
            raise RuntimeError("peer did not report the environment packet identity")

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

    required_geometry_targets = (0, 2)
    if len(gl_gbuffer) != 4 or len(vk_gbuffer) != 4 or not all(
            gl_gbuffer[index] and vk_gbuffer[index]
            for index in required_geometry_targets):
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
    if environment_hash is not None:
        for name in ("environment-draws", "water-draws", "water-underwater"):
            if token(gl_output, name) != token(vk_output, name):
                raise RuntimeError(
                    f"peer environment evidence differs for {name}")
        gl_environment = integers(gl_output, "environment-coverage")
        vk_environment = integers(vk_output, "environment-coverage")
        gl_water_modified = integers(gl_output, "water-modified")
        vk_water_modified = integers(vk_output, "water-modified")
        if len(gl_environment) != 4 or len(vk_environment) != 4:
            raise RuntimeError("environment evidence has an unexpected shape")
        if len(gl_water_modified) != 1 or len(vk_water_modified) != 1:
            raise RuntimeError("water evidence has an unexpected shape")
        if not gl_water_modified[0] or not vk_water_modified[0]:
            raise RuntimeError(
                "the paired capture contains no visible water contribution")
        passed &= compare_coverage(
            "environment", gl_environment, vk_environment,
            args.absolute_coverage_tolerance, args.relative_coverage_tolerance)
        passed &= compare_coverage(
            "water-modified", gl_water_modified, vk_water_modified,
            args.absolute_coverage_tolerance, args.relative_coverage_tolerance)
        if args.visible_reference is not None:
            passed &= compare_visible_water(
                args.visible_reference, gl_water,
                args.minimum_water_mask_iou, args.maximum_water_delta_mae)
            passed &= compare_visible_water(
                args.visible_reference, vk_water,
                args.minimum_water_mask_iou, args.maximum_water_delta_mae)
    if not passed:
        print("P0e1 production-frame peer comparison FAIL", file=sys.stderr)
        return 1

    print(
        "P0 production-frame peer comparison PASS "
        f"packet-sha256={packet_hash} "
        f"environment-sha256={environment_hash or 'none'} "
        f"absolute-coverage-tolerance={args.absolute_coverage_tolerance} "
        f"relative-coverage-tolerance={args.relative_coverage_tolerance}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"compare_ghi_production_frame: {error}", file=sys.stderr)
        sys.exit(2)

#!/usr/bin/env python3
"""Replay one production alpha packet on both native GHI peers."""

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


def records(output: str, backend: str) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for line in output.splitlines():
        if "P0e3c production alpha replay PASS" not in line:
            continue
        values: dict[str, str] = {}
        for item in line.split():
            if "=" in item:
                name, value = item.split("=", 1)
                values[name] = value
        if values.get("backend") == backend:
            result.append(values)
    if not result:
        raise RuntimeError(f"{backend} peer omitted PASS evidence")
    return result


def integer(record: dict[str, str], name: str) -> int:
    try:
        return int(record[name])
    except (KeyError, ValueError) as error:
        raise RuntimeError(f"peer output has invalid {name}") from error


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packet", type=pathlib.Path, required=True)
    parser.add_argument("--opengl", type=pathlib.Path, required=True)
    parser.add_argument("--vulkan", type=pathlib.Path, required=True)
    parser.add_argument("--absolute-coverage-tolerance", type=int, default=4)
    parser.add_argument("--relative-coverage-tolerance", type=float, default=0.001)
    parser.add_argument("--require-residual", action="store_true")
    parser.add_argument("--allow-deferred", action="store_true")
    parser.add_argument("--no-validation", action="store_true")
    args = parser.parse_args()

    for path in (args.packet, args.opengl, args.vulkan):
        if not path.is_file():
            parser.error(f"file not found: {path}")
    if args.absolute_coverage_tolerance < 0:
        parser.error("--absolute-coverage-tolerance must not be negative")
    if not 0.0 <= args.relative_coverage_tolerance <= 1.0:
        parser.error("--relative-coverage-tolerance must be between 0 and 1")

    packet_hash = hashlib.sha256(args.packet.read_bytes()).hexdigest()
    gl_output = run([str(args.opengl), "--packet", str(args.packet)])
    vk_command = [str(args.vulkan), "--packet", str(args.packet)]
    if not args.no_validation:
        vk_command.append("--validation")
    vk_output = run(vk_command)

    gl_records = records(gl_output, "OpenGL")
    if len(gl_records) != 2 or {item.get("profile") for item in gl_records} != {
            "OpenGL41", "OpenGL44"}:
        raise RuntimeError("OpenGL peer did not execute both 4.1 and 4.4 profiles")
    vk_record = records(vk_output, "Vulkan")[0]
    gl_record = gl_records[0]

    for record in (*gl_records, vk_record):
        if record.get("packet-sha256") != packet_hash:
            raise RuntimeError("peer did not report the input packet identity")
        for name in ("frame", "routes", "deferred-reasons"):
            if name not in record:
                raise RuntimeError(f"peer output omitted {name}")
        if integer(record, "modified") <= 0:
            raise RuntimeError("peer alpha replay changed no pixels")

    for name in ("frame", "routes", "deferred-reasons", "modified", "color-sha256"):
        if gl_records[0][name] != gl_records[1][name]:
            raise RuntimeError(f"OpenGL 4.1/4.4 evidence differs for {name}")
    for name in ("frame", "routes", "deferred-reasons"):
        if gl_record[name] != vk_record[name]:
            raise RuntimeError(f"native peer structural evidence differs for {name}")

    try:
        routes = [int(value) for value in gl_record["routes"].split(",")]
        reasons = [int(value) for value in gl_record["deferred-reasons"].split(",")]
    except ValueError as error:
        raise RuntimeError("peer route evidence is malformed") from error
    if len(routes) != 5 or len(reasons) != 3:
        raise RuntimeError("peer route evidence has an unexpected shape")
    mask, sorted_draws, residual, emissive, deferred = routes
    if not mask or not sorted_draws:
        raise RuntimeError("packet lacks executable mask or sorted-alpha evidence")
    if args.require_residual and not residual:
        raise RuntimeError("packet lacks required residual alpha evidence")
    if emissive > sorted_draws + residual:
        raise RuntimeError("emissive replay count exceeds executable alpha draws")
    if sum(reasons) != deferred:
        raise RuntimeError("deferred reason counts do not match deferred draws")
    if deferred and not args.allow_deferred:
        raise RuntimeError("production alpha replay deferred captured draws")

    gl_modified = integer(gl_record, "modified")
    vk_modified = integer(vk_record, "modified")
    delta = abs(gl_modified - vk_modified)
    allowed = max(
        args.absolute_coverage_tolerance,
        math.ceil(max(gl_modified, vk_modified) * args.relative_coverage_tolerance),
    )
    print(
        f"modified: opengl={gl_modified} vulkan={vk_modified} "
        f"delta={delta} allowed={allowed}")
    if delta > allowed:
        print("P0e3c production alpha peer comparison FAIL", file=sys.stderr)
        return 1

    print(
        "P0e3c production alpha peer comparison PASS "
        f"packet-sha256={packet_hash} routes={gl_record['routes']} "
        f"deferred-reasons={gl_record['deferred-reasons']} "
        f"absolute-coverage-tolerance={args.absolute_coverage_tolerance} "
        f"relative-coverage-tolerance={args.relative_coverage_tolerance}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"compare_ghi_production_alpha: {error}", file=sys.stderr)
        sys.exit(2)

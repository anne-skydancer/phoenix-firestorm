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
        if "production alpha replay PASS" not in line and \
            "production PPLL replay PASS" not in line:
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
    parser.add_argument("--ppll", action="store_true")
    parser.add_argument("--ppll-stress", action="store_true")
    parser.add_argument("--ppll-tail", action="store_true")
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
    gl_command = [str(args.opengl), "--packet", str(args.packet)]
    vk_command = [str(args.vulkan), "--packet", str(args.packet)]
    exact_requested = args.ppll or args.ppll_stress or args.ppll_tail
    if exact_requested:
        option = "--ppll-stress" if args.ppll_stress else \
            "--ppll-tail" if args.ppll_tail else "--ppll"
        gl_command.append(option)
        vk_command.append(option)
    gl_output = run(gl_command)
    if not args.no_validation:
        vk_command.append("--validation")
    vk_output = run(vk_command)

    gl_records = records(gl_output, "OpenGL")
    if len(gl_records) != 2 or {item.get("profile") for item in gl_records} != {
            "OpenGL41", "OpenGL44"}:
        raise RuntimeError("OpenGL peer did not execute both 4.1 and 4.4 profiles")
    vk_record = records(vk_output, "Vulkan")[0]

    for record in (*gl_records, vk_record):
        if record.get("source-sha256") != packet_hash:
            raise RuntimeError("peer did not report the source packet identity")
        for name in ("frame", "packet-sha256", "routes", "deferred-reasons", "ppll"):
            if name not in record:
                raise RuntimeError(f"peer output omitted {name}")
        if integer(record, "modified") <= 0:
            raise RuntimeError("peer alpha replay changed no pixels")

    gl44 = next(record for record in gl_records if record.get("profile") == "OpenGL44")
    gl41 = next(record for record in gl_records if record.get("profile") == "OpenGL41")
    legacy_equal = ("frame", "packet-sha256", "routes", "deferred-reasons",
                    "ppll", "modified", "color-sha256")
    if not exact_requested:
        for name in legacy_equal:
            if gl44[name] != gl41[name]:
                raise RuntimeError(f"OpenGL 4.1/4.4 evidence differs for {name}")
    for name in ("frame", "packet-sha256", "routes", "deferred-reasons"):
        if gl44[name] != vk_record[name]:
            raise RuntimeError(f"native peer structural evidence differs for {name}")

    try:
        routes = [int(value) for value in gl44["routes"].split(",")]
        reasons = [int(value) for value in gl44["deferred-reasons"].split(",")]
        ppll = [int(value) for value in gl44["ppll"].split(",")]
        vk_ppll = [int(value) for value in vk_record["ppll"].split(",")]
        fallback_routes = [int(value) for value in gl41["routes"].split(",")]
        fallback_ppll = [int(value) for value in gl41["ppll"].split(",")]
    except ValueError as error:
        raise RuntimeError("peer route evidence is malformed") from error
    if len(routes) != 5 or len(reasons) != 3 or len(ppll) != 6 or len(vk_ppll) != 6:
        raise RuntimeError("peer route evidence has an unexpected shape")
    mask, sorted_draws, residual, emissive, deferred = routes
    exact_draws = ppll[1]
    if not mask or not (sorted_draws or exact_draws):
        raise RuntimeError("packet lacks executable mask or alpha evidence")
    if args.require_residual and not residual:
        raise RuntimeError("packet lacks required residual alpha evidence")
    if emissive > sorted_draws + exact_draws + residual:
        raise RuntimeError("emissive replay count exceeds executable alpha draws")
    if sum(reasons) != deferred:
        raise RuntimeError("deferred reason counts do not match deferred draws")
    if deferred and not args.allow_deferred:
        raise RuntimeError("production alpha replay deferred captured draws")
    if exact_requested:
        available, exact_draws, capacity, exact_layers, allocated, overflow = ppll
        vk_available, vk_exact_draws, vk_capacity, vk_exact_layers, \
            vk_allocated, vk_overflow = vk_ppll
        if (available, exact_draws, capacity, exact_layers) != \
                (vk_available, vk_exact_draws, vk_capacity, vk_exact_layers):
            raise RuntimeError("native PPLL policy or route evidence differs")
        if not available or not exact_draws or not capacity or not exact_layers:
            raise RuntimeError("PPLL replay omitted exact-method work or bounds")
        if allocated < exact_draws or overflow > allocated:
            raise RuntimeError("PPLL allocation/overflow evidence is invalid")
        if args.ppll_stress and not overflow:
            raise RuntimeError("PPLL stress replay produced no overflow")
        if args.ppll_tail:
            if overflow:
                raise RuntimeError("PPLL weighted-tail replay unexpectedly overflowed")
            if exact_layers != 4 or allocated <= 64 * 64 * exact_layers:
                raise RuntimeError("PPLL weighted-tail replay did not exceed exact layers")
        if len(fallback_routes) != 5 or len(fallback_ppll) != 6:
            raise RuntimeError("OpenGL 4.1 fallback evidence is malformed")
        if fallback_ppll[0] or fallback_ppll[1]:
            raise RuntimeError("OpenGL 4.1 fallback incorrectly enabled PPLL")
        if fallback_routes[1] != routes[1] + exact_draws:
            raise RuntimeError("OpenGL 4.1 fallback did not retain exact draws as sorted")
        for name, gl_value, vk_value in (
                ("ppll-allocated", allocated, vk_allocated),
                ("ppll-overflow", overflow, vk_overflow)):
            delta = abs(gl_value - vk_value)
            allowed = max(
                args.absolute_coverage_tolerance,
                math.ceil(max(gl_value, vk_value) * args.relative_coverage_tolerance),
            )
            print(
                f"{name}: opengl={gl_value} vulkan={vk_value} "
                f"delta={delta} allowed={allowed}")
            if delta > allowed:
                raise RuntimeError(f"native {name} evidence exceeds tolerance")

    gl_modified = integer(gl44, "modified")
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
        f"P0e3{'d' if exact_requested else 'c'} production alpha peer comparison PASS "
        f"source-sha256={packet_hash} packet-sha256={gl44['packet-sha256']} "
        f"routes={gl44['routes']} ppll={gl44['ppll']} "
        f"deferred-reasons={gl44['deferred-reasons']} "
        f"absolute-coverage-tolerance={args.absolute_coverage_tolerance} "
        f"relative-coverage-tolerance={args.relative_coverage_tolerance}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(f"compare_ghi_production_alpha: {error}", file=sys.stderr)
        sys.exit(2)

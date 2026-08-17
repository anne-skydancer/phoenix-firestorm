#!/usr/bin/env python3
"""Build one deterministic GHI shader package.

The viewer never invokes these tools. Vulkan GLSL is compiled, optimized,
validated, and reflected offline; OpenGL sources are syntax checked and stored
beside the SPIR-V in a target-profile keyed package.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from typing import Any


PACKER_RECIPE = "ghi-shader-package-v3;glslang-vulkan1.3;spirv-opt-O;reflect-v1"
STAGE_SUFFIX = {"vertex": "vert", "fragment": "frag", "compute": "comp"}
STAGE_REFLECT = {"vertex": "VS", "fragment": "PS", "compute": "CS"}
DESCRIPTOR_TYPES = {
    "VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER": "uniform_buffer",
    "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER": "storage_buffer",
    "VK_DESCRIPTOR_TYPE_SAMPLER": "sampler",
    "VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE": "sampled_image",
    "VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER": "combined_image_sampler",
    "VK_DESCRIPTOR_TYPE_STORAGE_IMAGE": "storage_image",
}
SHADER_VALUE_TYPES = {
    "float": "float",
    "float2": "float2",
    "float3": "float3",
    "float4": "float4",
    "uint": "uint",
    "uint2": "uint2",
    "uint3": "uint3",
    "uint4": "uint4",
    "int": "sint",
    "int2": "sint2",
    "int3": "sint3",
    "int4": "sint4",
}


def fail(message: str) -> None:
    raise RuntimeError(message)


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command, check=False, text=True, encoding="utf-8", errors="replace",
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    if completed.returncode:
        fail(f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stdout}")
    return completed.stdout


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_json(value: Any) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode("utf-8")


def normalized_text(path: pathlib.Path) -> bytes:
    # read_text uses universal-newline translation, making packaged GLSL and
    # semantic hashes independent of checkout line-ending policy.
    return path.read_text(encoding="utf-8").encode("utf-8")


def first_line(text: str) -> str:
    return text.splitlines()[0].strip() if text.splitlines() else ""


def require_tool_version(tool: pathlib.Path, expected: str, glslang: bool = False) -> str:
    output = run([str(tool), "--version"])
    actual = ""
    if glslang:
        match = re.search(r"^Glslang Version:\s*(.+)$", output, re.MULTILINE)
        actual = match.group(1).strip() if match else ""
    else:
        actual = first_line(output)
    if actual != expected:
        fail(f"{tool.name} version mismatch: expected '{expected}', got '{actual}'")
    return output


def section_blocks(text: str, heading: str, item_pattern: str) -> list[str]:
    match = re.search(rf"^  {re.escape(heading)}:\s*\d+\s*$", text, re.MULTILINE)
    if not match:
        return []
    tail = text[match.end():]
    next_heading = re.search(r"^  [A-Za-z][^\n]+:\s*\d+\s*$", tail, re.MULTILINE)
    section = tail[:next_heading.start()] if next_heading else tail
    starts = list(re.finditer(item_pattern, section, re.MULTILINE))
    return [section[item.end(): starts[index + 1].start() if index + 1 < len(starts) else len(section)]
            for index, item in enumerate(starts)]


def field(block: str, name: str) -> str:
    match = re.search(rf"^\s+{re.escape(name)}\s*:\s*(.*?)\s*$", block, re.MULTILINE)
    if not match:
        fail(f"SPIRV-Reflect output omitted '{name}' in:\n{block}")
    return match.group(1).strip()


def reflect_module(reflector: pathlib.Path, spirv: pathlib.Path, stage: str) -> dict[str, Any]:
    text = run([str(reflector), str(spirv)])
    entry = re.search(r"^entry point\s*:\s*(\S+) \(stage=(\S+)\)$", text, re.MULTILINE)
    if not entry or entry.group(2) != STAGE_REFLECT[stage]:
        fail(f"SPIR-V stage mismatch for {spirv}: expected {STAGE_REFLECT[stage]}")

    def variables(heading: str) -> list[dict[str, Any]]:
        result = []
        for block in section_blocks(text, heading, r"^    \d+:\s*$"):
            location = field(block, "location")
            if not location.isdigit():
                continue
            result.append({
                "location": int(location),
                "type": field(block, "type"),
                "name": field(block, "name"),
            })
        return sorted(result, key=lambda item: item["location"])

    bindings = []
    for block in section_blocks(text, "Descriptor bindings", r"^    Binding \d+\.\d+\s*$"):
        raw_type = field(block, "type").split()[0]
        if raw_type not in DESCRIPTOR_TYPES:
            fail(f"unsupported reflected descriptor type: {raw_type}")
        raw_name = field(block, "name")
        block_name = re.search(r"\(([^()]+)\)\s*$", raw_name)
        bindings.append({
            "group": int(field(block, "set")),
            "binding": int(field(block, "binding")),
            "type": DESCRIPTOR_TYPES[raw_type],
            "name": block_name.group(1) if block_name else raw_name,
            "array_count": int(field(block, "count")),
            "dynamic_offset": False,
            "stages": [stage],
        })

    push_heading = re.search(r"^  Push constant blocks:\s*(\d+)\s*$", text, re.MULTILINE)
    if push_heading and int(push_heading.group(1)):
        fail("push-constant reflection is not yet supported by package schema v3")
    return {
        "entry_point": entry.group(1),
        "inputs": variables("Input variables"),
        "outputs": variables("Output variables"),
        "bindings": bindings,
    }


def merge_bindings(modules: list[dict[str, Any]]) -> list[dict[str, Any]]:
    merged: dict[tuple[int, int], dict[str, Any]] = {}
    for module in modules:
        for binding in module["bindings"]:
            key = (binding["group"], binding["binding"])
            if key not in merged:
                merged[key] = binding.copy()
                continue
            current = merged[key]
            for property_name in ("type", "name", "array_count", "dynamic_offset"):
                if current[property_name] != binding[property_name]:
                    fail(f"incompatible cross-stage descriptor {key}: {property_name} differs")
            current["stages"] = sorted(set(current["stages"] + binding["stages"]))
    return [merged[key] for key in sorted(merged)]


def validate_interfaces(modules_by_stage: dict[str, dict[str, Any]]) -> None:
    if "vertex" not in modules_by_stage or "fragment" not in modules_by_stage:
        return
    vertex_outputs = {item["location"]: item["type"] for item in modules_by_stage["vertex"]["outputs"]}
    for item in modules_by_stage["fragment"]["inputs"]:
        if vertex_outputs.get(item["location"]) != item["type"]:
            fail(f"vertex/fragment interface mismatch at location {item['location']}")


def normalized_expected_binding(binding: dict[str, Any]) -> dict[str, Any]:
    return {
        "group": binding["group"], "binding": binding["binding"],
        "type": binding["type"], "name": binding["name"],
        "array_count": binding.get("array_count", 1),
        "dynamic_offset": binding.get("dynamic_offset", False),
        "stages": sorted(binding["stages"]),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--glslang", type=pathlib.Path, required=True)
    parser.add_argument("--spirv-val", type=pathlib.Path, required=True)
    parser.add_argument("--spirv-opt", type=pathlib.Path, required=True)
    parser.add_argument("--spirv-reflect", type=pathlib.Path, required=True)
    args = parser.parse_args()

    manifest_path = args.manifest.resolve()
    root = manifest_path.parent
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("schema_version") != 3:
        fail("only GHI shader package schema version 3 is supported")
    tools = manifest["toolchain"]
    glslang_version = require_tool_version(args.glslang, tools["glslang_version"], glslang=True)
    val_version = require_tool_version(args.spirv_val, tools["spirv_tools_version"])
    opt_version = require_tool_version(args.spirv_opt, tools["spirv_tools_version"])

    toolchain_identity = {
        "recipe": PACKER_RECIPE,
        "glslang_version_output": glslang_version,
        "spirv_val_version_output": val_version,
        "spirv_opt_version_output": opt_version,
        "glslang_sha256": sha256(args.glslang.read_bytes()),
        "spirv_val_sha256": sha256(args.spirv_val.read_bytes()),
        "spirv_opt_sha256": sha256(args.spirv_opt.read_bytes()),
        "spirv_reflect_sha256": sha256(args.spirv_reflect.read_bytes()),
    }
    toolchain_hash = sha256(canonical_json(toolchain_identity))

    # Shader semantics and compiler identity are deliberately separate. A
    # toolchain upgrade invalidates native caches without pretending that the
    # source-level shader interface changed.
    semantic_files = {}
    for dependency in manifest.get("dependencies", []):
        path = root / dependency
        semantic_files[dependency] = sha256(normalized_text(path))

    stage_packages = []
    reflected_modules = []
    modules_by_stage = {}
    with tempfile.TemporaryDirectory(prefix="ghi_shader_") as temporary:
        temp = pathlib.Path(temporary)
        for stage_desc in manifest["stages"]:
            stage = stage_desc["stage"]
            suffix = STAGE_SUFFIX.get(stage)
            if not suffix:
                fail(f"unsupported shader stage: {stage}")
            artifacts = []
            for key, target in (("opengl_41", "opengl_41"), ("opengl_44", "opengl_44")):
                source_path = root / stage_desc[key]
                source = normalized_text(source_path)
                semantic_files[stage_desc[key]] = sha256(source)
                run([str(args.glslang), "-S", suffix, str(source_path)])
                artifacts.append({
                    "target": target, "encoding": "utf8",
                    "artifact_hash": sha256(source),
                    "source": source.decode("utf-8"),
                })

            source_path = root / stage_desc["vulkan"]
            semantic_files[stage_desc["vulkan"]] = sha256(normalized_text(source_path))
            unoptimized = temp / f"{stage}.unoptimized.spv"
            optimized = temp / f"{stage}.spv"
            run([
                str(args.glslang), "-V", "--target-env", tools["vulkan_target"],
                "-S", suffix, "-e", stage_desc["entry_point"],
                f"-I{source_path.parent}", "-o", str(unoptimized), str(source_path),
            ])
            run([str(args.spirv_val), "--target-env", tools["vulkan_target"], str(unoptimized)])
            run([str(args.spirv_opt), f"--target-env={tools['vulkan_target']}", "-O",
                 str(unoptimized), "-o", str(optimized)])
            run([str(args.spirv_val), "--target-env", tools["vulkan_target"], str(optimized)])
            reflected = reflect_module(args.spirv_reflect, optimized, stage)
            if reflected["entry_point"] != stage_desc["entry_point"]:
                fail(f"entry point mismatch in {source_path}")
            reflected_modules.append(reflected)
            modules_by_stage[stage] = reflected
            spirv = optimized.read_bytes()
            artifacts.append({
                "target": "vulkan_spirv_1_3", "encoding": "base64",
                "artifact_hash": sha256(spirv),
                "spirv": base64.b64encode(spirv).decode("ascii"),
            })
            stage_packages.append({
                "stage": stage, "entry_point": stage_desc["entry_point"],
                "artifacts": artifacts,
            })

    validate_interfaces(modules_by_stage)
    bindings = merge_bindings(reflected_modules)
    vertex_inputs = []
    for item in modules_by_stage.get("vertex", {}).get("inputs", []):
        if item["type"] not in SHADER_VALUE_TYPES:
            fail(f"unsupported reflected vertex type: {item['type']}")
        vertex_inputs.append({"location": item["location"], "type": SHADER_VALUE_TYPES[item["type"]]})

    fragment_outputs = []
    for item in modules_by_stage.get("fragment", {}).get("outputs", []):
        if item["type"] not in SHADER_VALUE_TYPES:
            fail(f"unsupported reflected fragment output type: {item['type']}")
        fragment_outputs.append({"location": item["location"], "type": SHADER_VALUE_TYPES[item["type"]]})

    expected = manifest["expected"]
    expected_bindings = sorted((normalized_expected_binding(item) for item in expected["bindings"]),
                               key=lambda item: (item["group"], item["binding"]))
    if bindings != expected_bindings:
        fail(f"reflected bindings differ from manifest\nexpected: {expected_bindings}\nactual:   {bindings}")
    if vertex_inputs != expected["vertex_inputs"]:
        fail(f"reflected vertex inputs differ from manifest: {vertex_inputs}")
    if fragment_outputs != expected["fragment_outputs"]:
        fail(f"reflected fragment outputs differ from manifest: {fragment_outputs}")

    semantic_identity = {
        "schema_version": manifest["schema_version"],
        "name": manifest["name"],
        "stages": [{key: stage[key] for key in
                    ("stage", "entry_point", "opengl_41", "opengl_44", "vulkan")}
                   for stage in manifest["stages"]],
        "files": semantic_files,
        "expected": expected,
    }
    package = {
        "schema_version": 3,
        "name": manifest["name"],
        "semantic_hash": sha256(canonical_json(semantic_identity)),
        "toolchain_hash": toolchain_hash,
        "toolchain": toolchain_identity,
        "stages": stage_packages,
        "bindings": bindings,
        "vertex_inputs": vertex_inputs,
        "fragment_outputs": fragment_outputs,
        "push_constant_bytes": expected["push_constant_bytes"],
    }
    output_bytes = canonical_json(package)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output_bytes)
    print(f"{args.output}: {sha256(output_bytes)}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyError, OSError, RuntimeError, ValueError) as error:
        print(f"pack_ghi_shader: {error}", file=sys.stderr)
        sys.exit(1)

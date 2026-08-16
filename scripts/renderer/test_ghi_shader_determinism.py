#!/usr/bin/env python3
"""Build a GHI package twice and require byte-identical output."""

from __future__ import annotations

import argparse
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--packer", type=pathlib.Path, required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    parser.add_argument("--glslang", type=pathlib.Path, required=True)
    parser.add_argument("--spirv-val", type=pathlib.Path, required=True)
    parser.add_argument("--spirv-opt", type=pathlib.Path, required=True)
    parser.add_argument("--spirv-reflect", type=pathlib.Path, required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ghi_determinism_") as temporary:
        temporary_path = pathlib.Path(temporary)
        shader_root = temporary_path / "shader"
        shutil.copytree(args.manifest.parent, shader_root)
        test_manifest = shader_root / args.manifest.name
        outputs = [temporary_path / "first.llghisp", temporary_path / "second.llghisp"]
        for output in outputs:
            command = [
                sys.executable, str(args.packer),
                "--manifest", str(test_manifest), "--output", str(output),
                "--glslang", str(args.glslang),
                "--spirv-val", str(args.spirv_val),
                "--spirv-opt", str(args.spirv_opt),
                "--spirv-reflect", str(args.spirv_reflect),
            ]
            completed = subprocess.run(command, check=False)
            if completed.returncode:
                return completed.returncode
        if outputs[0].read_bytes() != outputs[1].read_bytes():
            print("GHI shader packages are not deterministic", file=sys.stderr)
            return 1

        malformed = json.loads(test_manifest.read_text(encoding="utf-8"))
        expected = malformed["expected"]
        if expected.get("bindings"):
            expected["bindings"][0]["binding"] = 7
        elif expected.get("vertex_inputs"):
            expected["vertex_inputs"][0]["location"] = 7
        elif expected.get("fragment_outputs"):
            expected["fragment_outputs"][0]["location"] = 7
        else:
            print("manifest has no reflected interface to corrupt", file=sys.stderr)
            return 1
        test_manifest.write_text(json.dumps(malformed), encoding="utf-8")
        negative_output = temporary_path / "must_not_build.llghisp"
        negative_command = [
            sys.executable, str(args.packer),
            "--manifest", str(test_manifest), "--output", str(negative_output),
            "--glslang", str(args.glslang),
            "--spirv-val", str(args.spirv_val),
            "--spirv-opt", str(args.spirv_opt),
            "--spirv-reflect", str(args.spirv_reflect),
        ]
        negative = subprocess.run(
            negative_command, check=False, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        )
        if negative.returncode == 0 or negative_output.exists():
            print("reflection/manifest mismatch was not rejected", file=sys.stderr)
            return 1
    print("GHI shader package is byte deterministic")
    print("GHI shader reflection mismatch is rejected")
    return 0


if __name__ == "__main__":
    sys.exit(main())

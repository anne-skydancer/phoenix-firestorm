#!/usr/bin/env python3
"""Create and validate durable VulkanStorm GHI evidence manifests."""

from argparse import ArgumentParser
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


SCHEMA_VERSION = 1
GATE_PATTERN = re.compile(r"^(?:P0[a-g](?:[0-9a-z.]*)?|P0\.8)$")
RESULTS = {"accepted", "failed", "pending", "skipped"}
REQUIRED_ENVIRONMENT = {"backend", "adapter", "driver", "resolution", "toolchain"}


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as artifact:
        for block in iter(lambda: artifact.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def revision(repository):
    result = subprocess.run(
        ["git", "rev-parse", "HEAD"],
        cwd=repository,
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip()


def parse_pairs(values, option):
    pairs = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"{option} requires KEY=VALUE, got {value!r}")
        key, item = value.split("=", 1)
        if not key or not item:
            raise ValueError(f"{option} requires nonempty KEY=VALUE, got {value!r}")
        pairs[key] = item
    return pairs


def artifact_record(path, root):
    resolved = path.resolve()
    try:
        stored_path = resolved.relative_to(root.resolve()).as_posix()
    except ValueError as error:
        raise ValueError(f"artifact is outside bundle root: {path}") from error
    return {
        "path": stored_path,
        "sha256": sha256(resolved),
        "size": resolved.stat().st_size,
    }


def create_manifest(args):
    root = args.root.resolve()
    environment = parse_pairs(args.environment, "--environment")
    missing = sorted(REQUIRED_ENVIRONMENT - environment.keys())
    if missing:
        raise ValueError(f"missing environment keys: {', '.join(missing)}")

    criteria = []
    for value in args.criterion:
        if "=" not in value:
            raise ValueError(f"--criterion requires NAME=pass|fail, got {value!r}")
        name, outcome = value.rsplit("=", 1)
        if not name or outcome not in {"pass", "fail"}:
            raise ValueError(f"--criterion requires NAME=pass|fail, got {value!r}")
        criteria.append({"name": name, "passed": outcome == "pass"})

    manifest = {
        "schema_version": SCHEMA_VERSION,
        "gate": args.gate,
        "result": args.result,
        "revision": args.revision or revision(args.repository),
        "created_at_utc": datetime.now(timezone.utc).isoformat(),
        "environment": environment,
        "criteria": criteria,
        "artifacts": [artifact_record(path, root) for path in args.artifact],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return validate_manifest(args.output, root)


def require(condition, message, errors):
    if not condition:
        errors.append(message)


def validate_manifest(path, root):
    manifest = json.loads(path.read_text(encoding="utf-8"))
    errors = []
    require(manifest.get("schema_version") == SCHEMA_VERSION, "unsupported schema_version", errors)
    require(bool(GATE_PATTERN.fullmatch(manifest.get("gate", ""))), "invalid gate", errors)
    require(manifest.get("result") in RESULTS, "invalid result", errors)
    require(bool(manifest.get("revision")), "missing revision", errors)

    environment = manifest.get("environment", {})
    require(isinstance(environment, dict), "environment must be an object", errors)
    if isinstance(environment, dict):
        missing = sorted(REQUIRED_ENVIRONMENT - environment.keys())
        require(not missing, f"missing environment keys: {', '.join(missing)}", errors)

    criteria = manifest.get("criteria", [])
    require(isinstance(criteria, list), "criteria must be an array", errors)
    if isinstance(criteria, list):
        for index, criterion in enumerate(criteria):
            require(isinstance(criterion, dict), f"criterion {index} must be an object", errors)
            if isinstance(criterion, dict):
                require(bool(criterion.get("name")), f"criterion {index} has no name", errors)
                require(isinstance(criterion.get("passed"), bool), f"criterion {index} has invalid passed value", errors)

    artifacts = manifest.get("artifacts")
    require(isinstance(artifacts, list), "artifacts must be an array", errors)
    if isinstance(artifacts, list):
        for index, artifact in enumerate(artifacts):
            if not isinstance(artifact, dict):
                errors.append(f"artifact {index} must be an object")
                continue
            artifact_path = root / artifact.get("path", "")
            require(artifact_path.is_file(), f"artifact {index} is missing: {artifact_path}", errors)
            if artifact_path.is_file():
                require(artifact.get("size") == artifact_path.stat().st_size, f"artifact {index} size differs", errors)
                require(artifact.get("sha256") == sha256(artifact_path), f"artifact {index} hash differs", errors)

    if errors:
        raise ValueError("\n".join(errors))
    print(f"Validated GHI evidence manifest: {path}")
    return 0


def build_parser():
    parser = ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    create = subparsers.add_parser("create", help="create and validate a manifest")
    create.add_argument("--gate", required=True)
    create.add_argument("--result", choices=sorted(RESULTS), required=True)
    create.add_argument("--root", type=Path, required=True, help="evidence bundle root")
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--repository", type=Path, default=Path.cwd())
    create.add_argument("--revision")
    create.add_argument("--environment", action="append", default=[], metavar="KEY=VALUE")
    create.add_argument("--criterion", action="append", default=[], metavar="NAME=pass|fail")
    create.add_argument("--artifact", action="append", default=[], type=Path)

    validate = subparsers.add_parser("validate", help="validate a manifest and its artifacts")
    validate.add_argument("manifest", type=Path)
    validate.add_argument("--root", type=Path, required=True, help="evidence bundle root")
    return parser


def main(raw_args=None):
    args = build_parser().parse_args(raw_args)
    try:
        if args.command == "create":
            return create_manifest(args)
        return validate_manifest(args.manifest, args.root.resolve())
    except (OSError, ValueError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
#!/usr/bin/env python3
"""\
@file   check_render_api_boundary.py
@author Vulkanstorm contributors
@date   2026-08-12
@brief  Prevent growth of legacy graphics-API coupling during RHI migration.

$LicenseInfo:firstyear=2026&license=fsviewerlgpl$
Phoenix Firestorm Viewer Source Code
Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
$/LicenseInfo$
"""

from argparse import ArgumentParser
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import subprocess
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BASELINE = REPOSITORY_ROOT / "doc" / "renderer" / "gl-coupling-baseline.json"
SCANNED_ROOTS = (
    "indra/llappearance",
    "indra/llrender",
    "indra/llui",
    "indra/llwindow",
    "indra/newview",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".m", ".mm"}
HEADER_SUFFIXES = {".h", ".hpp", ".inl"}

# New native API code belongs only in these backend directories. They need not
# exist yet; declaring the boundary now prevents another API from leaking into
# renderer-facing code when implementation begins.
EXCLUDED_BACKEND_PREFIXES = (
    "indra/llrender/rhi/gl/",
    "indra/llrender/rhi/vulkan/",
)

METRICS = {
    "direct_gl_calls": {
        "description": "Raw OpenGL function calls",
        "pattern": re.compile(r"\bgl[A-Z][A-Za-z0-9_]*\s*\("),
    },
    "ggl_facade_uses": {
        "description": "Uses of the legacy global gGL state facade",
        "pattern": re.compile(r"\bgGL\s*\."),
    },
    "llgl_state_uses": {
        "description": "Uses of legacy LLGL state wrappers",
        "pattern": re.compile(
            r"\bLLGL(?:State|DepthTest|StencilTest|Disable|Enable)\b"
        ),
    },
    "gl_types_in_headers": {
        "description": "OpenGL types exposed by headers",
        "pattern": re.compile(
            r"\b(?:GLenum|GLuint|GLint|GLfloat|GLdouble|GLboolean|GLsizei|"
            r"GLbitfield|GLchar|GLsync|GLintptr|GLsizeiptr)\b"
        ),
        "suffixes": HEADER_SUFFIXES,
    },
    "gl_header_includes": {
        "description": "Direct OpenGL header includes",
        "pattern": re.compile(
            r"^\s*#\s*include\s*[<\"][^>\"]*(?:GL/|OpenGL/|glew|glad)",
            re.MULTILINE | re.IGNORECASE,
        ),
        "scan_raw_source": True,
    },
    "direct_vulkan_calls": {
        "description": "Vulkan calls outside the Vulkan backend",
        "pattern": re.compile(r"\bvk[A-Z][A-Za-z0-9_]*\s*\("),
    },
    "vulkan_types_in_headers": {
        "description": "Vulkan types exposed outside the Vulkan backend",
        "pattern": re.compile(r"\bVk[A-Z][A-Za-z0-9_]*\b"),
        "suffixes": HEADER_SUFFIXES,
    },
    "vulkan_symbols": {
        "description": "Native Vulkan types, entry points, and macros outside the backend",
        "pattern": re.compile(
            r"\b(?:Vk[A-Z][A-Za-z0-9_]*|PFN_vk[A-Za-z0-9_]*|"
            r"VK_(?:API|MAKE|VERSION|SUCCESS|ERROR|STRUCTURE|FORMAT|IMAGE|"
            r"BUFFER|PIPELINE|ACCESS|QUEUE|MEMORY|SAMPLE|SHADER|DESCRIPTOR|"
            r"PRESENT|COLOR|CULL|FRONT|BLEND|COMPARE|ATTACHMENT|COMMAND|"
            r"FENCE|SEMAPHORE|NULL|TRUE|FALSE)[A-Z0-9_]*)\b"
        ),
    },
}

COMMENT_OR_LITERAL = re.compile(
    r"//[^\r\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)


def git_revision():
    """Return the checked-out revision without requiring GitPython."""
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=REPOSITORY_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        # Inventory checks do not depend on the current revision. This fallback
        # also lets exported source trees run the guard without a .git folder.
        return "unknown"


def excluded(relative_path):
    return any(relative_path.startswith(prefix)
               for prefix in EXCLUDED_BACKEND_PREFIXES)


def source_files():
    for root_name in SCANNED_ROOTS:
        root = REPOSITORY_ROOT / root_name
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(REPOSITORY_ROOT).as_posix()
            if not excluded(relative):
                yield path, relative


def collect_inventory():
    counts = {name: {} for name in METRICS}
    for path, relative in source_files():
        source = path.read_text(encoding="utf-8", errors="replace")
        code = COMMENT_OR_LITERAL.sub("", source)
        for name, specification in METRICS.items():
            suffixes = specification.get("suffixes", SOURCE_SUFFIXES)
            if path.suffix.lower() not in suffixes:
                continue
            scanned_source = source if specification.get("scan_raw_source") else code
            count = len(specification["pattern"].findall(scanned_source))
            if count:
                counts[name][relative] = count

    return {
        "schema": 1,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "baseline_commit": git_revision(),
        "policy": "Per-file counts may decrease but may not increase.",
        "scope": {
            "roots": list(SCANNED_ROOTS),
            "excluded_backend_prefixes": list(EXCLUDED_BACKEND_PREFIXES),
        },
        "metrics": {
            name: {
                "description": METRICS[name]["description"],
                "total": sum(files.values()),
                "files": dict(sorted(files.items())),
            }
            for name, files in counts.items()
        },
    }


def print_summary(inventory, baseline=None):
    print("Renderer API coupling inventory")
    for name, metric in inventory["metrics"].items():
        difference = ""
        if baseline:
            previous = baseline["metrics"][name]["total"]
            difference = f" ({metric['total'] - previous:+d})"
        print(f"  {name:25} {metric['total']:5d}{difference}")


def check_inventory(inventory, baseline):
    violations = []
    for name, current_metric in inventory["metrics"].items():
        allowed_files = baseline["metrics"].get(name, {}).get("files", {})
        for path, count in current_metric["files"].items():
            allowed = allowed_files.get(path, 0)
            if count > allowed:
                violations.append((name, path, allowed, count))

    if not violations:
        print("Render API boundary ratchet passed.")
        return 0

    print("Render API boundary ratchet failed:", file=sys.stderr)
    for name, path, allowed, count in violations:
        print(
            f"  {name}: {path}: allowed {allowed}, found {count} (+{count - allowed})",
            file=sys.stderr,
        )
    print(
        "Move native API work below the RHI backend boundary. Update the baseline "
        "only after an explicit architecture review.",
        file=sys.stderr,
    )
    return 1


def main(raw_args=None):
    parser = ArgumentParser(
        description="Inventory and ratchet legacy renderer API coupling."
    )
    parser.add_argument(
        "--baseline",
        type=Path,
        default=DEFAULT_BASELINE,
        help="baseline JSON path (default: %(default)s)",
    )
    parser.add_argument(
        "--write-baseline",
        action="store_true",
        help="replace the baseline with the current inventory",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if any per-file coupling count exceeds the baseline",
    )
    args = parser.parse_args(raw_args)

    inventory = collect_inventory()
    baseline_path = args.baseline

    if args.write_baseline:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(
            json.dumps(inventory, indent=2) + "\n", encoding="utf-8"
        )
        print_summary(inventory)
        print(f"Wrote {baseline_path.relative_to(REPOSITORY_ROOT)}")
        return 0

    if not baseline_path.exists():
        parser.error(f"baseline does not exist: {baseline_path}")

    baseline = json.loads(baseline_path.read_text(encoding="utf-8"))
    print_summary(inventory, baseline)
    return check_inventory(inventory, baseline) if args.check else 0


if __name__ == "__main__":
    sys.exit(main())

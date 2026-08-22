#!/usr/bin/env python3

import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys


SOLOUD_REPOSITORY = "https://github.com/jarikomppa/soloud.git"
SOLOUD_REVISION = "e82fd32c1f62183922f08c14c814a02b58db1873"
GROK_REPOSITORY = "https://github.com/GrokImageCompression/grok.git"
GROK_REVISION = "20ea98f31ace0b0f1413f40188923314f735478d"
GROK_SUBMODULES = (
    "src/include/CLI11",
    "src/include/spdlog",
    "src/include/taskflow",
    "src/lib/core/highway",
)
MESA_REPOSITORY = "https://gitlab.freedesktop.org/mesa/mesa.git"
MESA_REVISION = "00e42c51b10d8e0769489156fa414f111897d515"
MESA_VERSION = "26.3.0-devel"


def run(command: list[str], cwd: Path | None = None) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def patch_digest(patches: list[Path]) -> str:
    digest = hashlib.sha256()
    for patch in patches:
        digest.update(patch.read_bytes())
    return digest.hexdigest()


def remove_tree(path: Path) -> None:
    def remove_readonly(function, filename, error_info) -> None:
        os.chmod(filename, stat.S_IWRITE)
        function(filename)

    shutil.rmtree(path, onerror=remove_readonly)


def ensure_checkout(
    name: str,
    repository: str,
    revision: str,
    destination: Path,
    patches: list[Path],
) -> None:
    marker = destination / ".vulkanstorm-source.json"
    expected = {"repository": repository, "revision": revision, "patches": patch_digest(patches)}
    if marker.exists() and json.loads(marker.read_text(encoding="utf-8")) == expected:
        print(f"{name}: using prepared source at {destination}")
        return

    if destination.exists():
        remove_tree(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", "--filter=blob:none", "--no-checkout", repository, str(destination)])
    run(["git", "checkout", "--detach", revision], cwd=destination)
    for patch in patches:
        run(["git", "apply", "--check", "--ignore-space-change", str(patch)], cwd=destination)
        run(["git", "apply", "--ignore-space-change", str(patch)], cwd=destination)
    marker.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")


def ensure_python_build_tools() -> Path:
    modules = ("mesonbuild", "mako", "packaging", "yaml", "setuptools")
    if any(importlib.util.find_spec(module) is None for module in modules):
        run([sys.executable, "-m", "pip", "install", "meson>=1.4", "mako", "packaging", "pyyaml", "setuptools"])

    ninja = shutil.which("ninja")
    if ninja is None:
        run([sys.executable, "-m", "pip", "install", "ninja"])
        scripts = Path(sys.executable).parent / "Scripts"
        ninja_path = scripts / ("ninja.exe" if os.name == "nt" else "ninja")
        if not ninja_path.exists():
            raise RuntimeError("ninja was installed but its executable could not be located")
        return ninja_path
    return Path(ninja)


def build_grok(source: Path, generator: str | None, check_only: bool) -> None:
    run(["git", "submodule", "update", "--init", "--recursive", "--", *GROK_SUBMODULES], cwd=source)
    if check_only:
        return

    build_dir = source / "build"
    command = [
        "cmake", "-S", str(source), "-B", str(build_dir),
        "-DBUILD_SHARED_LIBS=ON",
        "-DGRK_BUILD_CODEC=OFF",
        "-DGRK_BUILD_LIBPNG=ON",
        "-DGRK_BUILD_LIBTIFF=ON",
        "-DGRK_BUILD_LCMS2=ON",
        "-DGRK_BUILD_JPEG=ON",
        "-DGRK_BUILD_CORE_EXAMPLES=OFF",
        "-DGRK_BUILD_CODEC_EXAMPLES=OFF",
        "-DGRK_BUILD_PLUGIN_LOADER=OFF",
        "-DGRK_BUILD_CORE_SWIG_BINDINGS=OFF",
        "-DGRK_BUILD_DOC=OFF",
        "-DBUILD_TESTING=OFF",
        f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE={build_dir / 'bin'}",
        f"-DCMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE={build_dir / 'bin'}",
        f"-DCMAKE_ARCHIVE_OUTPUT_DIRECTORY_RELEASE={build_dir / 'lib'}",
    ]
    if generator:
        command.extend(["-G", generator])
    run(command)
    run(["cmake", "--build", str(build_dir), "--config", "Release", "--target", "grokj2k"])


def build_mesa(source: Path, install_dir: Path, check_only: bool) -> None:
    version = (source / "VERSION").read_text(encoding="utf-8").strip()
    if version != MESA_VERSION:
        raise RuntimeError(f"Mesa revision declares {version}, expected {MESA_VERSION}")
    if check_only:
        return

    ninja = ensure_python_build_tools()
    build_dir = source / "build-vulkanstorm"
    if not (build_dir / "build.ninja").exists():
        run([
            sys.executable,
            "-m",
            "mesonbuild.mesonmain",
            "setup",
            str(build_dir),
            "-Dbuildtype=release",
            "-Dvsenv=true",
            "-Dgallium-drivers=zink",
            "-Dvulkan-drivers=",
            "-Dllvm=disabled",
            "-Dgles1=disabled",
            "-Dgles2=disabled",
            "-Dglx=disabled",
            "-Degl=disabled",
            "-Dmicrosoft-clc=disabled",
            "-Dzlib:default_library=static",
        ], cwd=source)
    run([str(ninja), "-C", str(build_dir)])

    release_dir = install_dir / "bin" / "release"
    license_dir = install_dir / "LICENSES"
    release_dir.mkdir(parents=True, exist_ok=True)
    license_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(build_dir / "src/gallium/targets/wgl/libgallium_wgl.dll", release_dir)
    shutil.copy2(build_dir / "src/gallium/targets/libgl-gdi/opengl32.dll", release_dir)
    shutil.copy2(source / "docs/license.rst", license_dir / "mesazink.txt")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Prepare VulkanStorm source dependencies")
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--install-dir", type=Path, required=True)
    parser.add_argument("--grok", action="store_true")
    parser.add_argument("--soloud", action="store_true")
    parser.add_argument("--mesazink", action="store_true")
    parser.add_argument("--cmake-generator")
    parser.add_argument("--check", action="store_true", help="fetch and patch sources without building Mesa")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    patch_dir = Path(__file__).resolve().parent / "dependency_patches"
    if args.grok:
        grok_source = args.root / "grok-src"
        ensure_checkout("Grok", GROK_REPOSITORY, GROK_REVISION, grok_source, [])
        build_grok(grok_source, args.cmake_generator, args.check)
    if args.soloud:
        ensure_checkout(
            "SoLoud",
            SOLOUD_REPOSITORY,
            SOLOUD_REVISION,
            args.root / "soloud-src",
            [patch_dir / "soloud-vulkanstorm.patch"],
        )
    if args.mesazink:
        mesa_source = args.root / "mesa-src"
        ensure_checkout(
            "Mesa",
            MESA_REPOSITORY,
            MESA_REVISION,
            mesa_source,
            [
                patch_dir / "mesa-zink-null-guards.patch",
                patch_dir / "mesa-msvc-release.patch",
            ],
        )
        build_mesa(mesa_source, args.install_dir, args.check)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
#!/usr/bin/env python3
"""Build and verify the P0 legacy OpenGL state/convergence ledger.

This is a source inventory, not a proof of runtime reachability.  It groups
every legacy gGL member access, known LLGL state wrapper reference, and raw
OpenGL-shaped call above the GHI backend boundary by semantic disposition and
source owner.  Runtime evidence is recorded separately before removal.
"""

from argparse import ArgumentParser
from collections import defaultdict
import json
from pathlib import Path
import re
import subprocess
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_LEDGER = REPOSITORY_ROOT / "doc" / "renderer" / "p0-legacy-gl-state-ledger.json"
SCANNED_ROOTS = (
    "indra/llappearance",
    "indra/llrender",
    "indra/llui",
    "indra/llwindow",
    "indra/newview",
)
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hpp", ".inl", ".m", ".mm"}
EXCLUDED_BACKEND_PREFIXES = (
    "indra/llrender/ghi/backends/opengl/",
    "indra/llrender/ghi/backends/vulkan/",
)
MACOS_GL_COMPATIBILITY_HEADER = "indra/llrender/llglheaders.h"

COMMENT_OR_LITERAL = re.compile(
    r"//[^\r\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'",
    re.DOTALL,
)
GGL_MEMBER = re.compile(r"\bgGL\s*\.\s*([A-Za-z_][A-Za-z0-9_]*)")
DIRECT_GL_CALL = re.compile(r"\b(gl[A-Z][A-Za-z0-9_]*)\s*\(")


GGL_GROUPS = {
    "draw_assembly": {
        "begin", "beginList", "color3f", "color3fv", "color4f", "color4fv",
        "color4ub", "color4ubv", "diffuseColor3f", "diffuseColor3fv",
        "diffuseColor4f", "diffuseColor4fv", "diffuseColor4ubv", "end",
        "endList", "normal3fv", "texCoord2f", "texCoord2fv", "texCoord2i",
        "vertex2f", "vertex2fv", "vertex2i", "vertex3f", "vertex3fv",
        "vertexBatchPreTransformed",
    },
    "matrix_and_ui_transform": {
        "getMatrixMode", "getModelviewMatrix", "getProjectionMatrix",
        "getUIScale", "getUITranslation", "loadIdentity", "loadMatrix",
        "loadUIIdentity", "matrixMode", "MM_MODELVIEW", "MM_PROJECTION",
        "multMatrix", "ortho", "popMatrix", "popUIMatrix", "pushMatrix",
        "pushUIMatrix", "rotatef", "scalef", "scaleUI", "syncMatrices",
        "translatef", "translateUI",
    },
    "pipeline_and_dynamic_state": {
        "blendFunc", "LINES", "POINTS", "setColorMask", "setLineWidth",
        "setSceneBlendType", "TRIANGLE_STRIP",
    },
    "resource_and_light_binding": {
        "getCurrentTexUnitIndex", "getLight", "getTexUnit", "mLightHash",
        "setAmbientLightColor",
    },
    "lifecycle_and_cache": {
        "flush", "init", "mCurrTextureUnitIndex", "mDirty", "refreshState",
        "shutdown",
    },
}

GGL_DISPOSITIONS = {
    "draw_assembly": "replace_with_explicit_draw_packets",
    "matrix_and_ui_transform": "move_to_explicit_frame_pass_or_draw_data",
    "pipeline_and_dynamic_state": "encode_in_ghi_pipeline_or_dynamic_state",
    "resource_and_light_binding": "replace_with_ghi_binding_sets",
    "lifecycle_and_cache": "replace_or_confine_to_backend_lifecycle",
}

LEGACY_STATE_SYMBOLS = {
    "LLGLState": ("live_implicit_state", "replace_with_ghi_state_owner"),
    "LLGLEnable": ("live_implicit_state", "replace_with_ghi_pipeline_state"),
    "LLGLDisable": ("live_implicit_state", "replace_with_ghi_pipeline_state"),
    "LLGLDepthTest": ("live_implicit_state", "replace_with_ghi_depth_state"),
    "LLGLEnableBlending": ("unreferenced_candidate", "remove_after_runtime_gate"),
    "LLGLEnableAlphaReject": ("unreferenced_candidate", "remove_after_runtime_gate"),
    "LLGLEnableFunc": ("unreferenced_candidate", "remove_after_runtime_gate"),
    "LLGLSDefault": ("live_state_bundle", "replace_with_explicit_pass_state"),
    "LLGLSObjectSelect": ("unreferenced_candidate", "remove_after_runtime_gate"),
    "LLGLSUIDefault": ("live_state_bundle", "replace_with_explicit_ui_pass_state"),
    "LLGLSPipeline": ("live_state_bundle", "replace_with_explicit_pass_state"),
    "LLGLSPipelineAlpha": ("live_state_bundle", "replace_with_explicit_alpha_state"),
    "LLGLSPipelineSelection": ("live_state_bundle", "replace_with_explicit_selection_state"),
    "LLGLSPipelineSkyBox": ("live_state_bundle", "replace_with_explicit_sky_state"),
    "LLGLSPipelineDepthTestSkyBox": ("live_state_bundle", "replace_with_explicit_sky_state"),
    "LLGLSPipelineBlendSkyBox": ("live_state_bundle", "replace_with_explicit_sky_state"),
    "LLGLSTracker": ("live_state_bundle", "replace_with_explicit_ui_state"),
    "LLGLSSpecular": ("dormant_fixed_function", "remove_or_replace_with_material_data"),
    "LLGLUserClipPlane": ("unreferenced_candidate", "remove_after_runtime_gate"),
    "LLGLSquashToFarClip": ("live_matrix_state", "replace_with_explicit_projection_data"),
    "LLGLSyncFence": ("unreferenced_candidate", "remove_after_runtime_gate"),
}

LEGACY_STATE_REFERENCE = re.compile(
    r"\b(" + "|".join(sorted(LEGACY_STATE_SYMBOLS, key=len, reverse=True)) + r")\b"
)

OBSOLETE_FIXED_FUNCTION_GL = {
    "glAlphaFunc", "glAreTexturesResident", "glClientActiveTexture",
    "glColor4ubv", "glDisableClientState", "glEnableClientState",
    "glLoadIdentity", "glLoadMatrixf", "glMaterialfv", "glMateriali",
    "glMatrixMode", "glPopAttrib", "glPopClientAttrib", "glPopMatrix",
    "glPushAttrib", "glPushClientAttrib", "glPushMatrix", "glTexCoordPointer",
    "glTexGenfv", "glTexGeni", "glVertexPointer",
}

LEGACY_EXTENSION_ALIASES = {
    "glBindBufferARB", "glBindFramebufferEXT", "glBindRenderbufferEXT",
    "glBlendFuncSeparateEXT", "glBufferDataARB", "glBufferSubDataARB",
    "glCheckFramebufferStatusEXT", "glDeleteBuffersARB",
    "glDeleteFramebuffersEXT", "glDeleteRenderbuffersEXT",
    "glFramebufferRenderbufferEXT", "glFramebufferTexture1DEXT",
    "glFramebufferTexture2DEXT", "glFramebufferTexture3DEXT",
    "glGenBuffersARB", "glGenerateMipmapEXT", "glGenFramebuffersEXT",
    "glGenRenderbuffersEXT", "glGetBufferParameterivARB",
    "glGetBufferPointervARB", "glGetBufferSubDataARB",
    "glGetFramebufferAttachmentParameterivEXT", "glGetRenderbufferParameterivEXT",
    "glIsBufferARB", "glIsFramebufferEXT", "glIsRenderbufferEXT",
    "glMapBufferARB", "glRenderbufferStorageEXT", "glUnmapBufferARB",
}

MODERN_STATE_GL = {
    "glBlendEquationSeparate", "glBlendEquationSeparatei", "glBlendFunc",
    "glBlendFuncSeparate", "glBlendFuncSeparatei", "glColorMask",
    "glColorMaski", "glCullFace", "glDepthFunc", "glDepthMask", "glDepthRange",
    "glDisable", "glDisablei", "glEnable", "glEnablei", "glFrontFace",
    "glLineWidth", "glPointSize", "glPolygonMode", "glPolygonOffset",
    "glScissor", "glStencilFunc", "glStencilFuncSeparate", "glStencilMask",
    "glStencilMaskSeparate", "glStencilOp", "glStencilOpSeparate", "glViewport",
}

# These match the intentionally broad direct-GL heuristic but are viewer-owned
# functions rather than OpenGL entry points.
PROJECT_GL_SHAPED_SYMBOLS = {
    "glPointToScreen", "glReady", "glRectToScreen", "glSwapBuffers",
}


def git_revision():
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=REPOSITORY_ROOT, check=True,
            capture_output=True, text=True,
        )
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def source_files():
    for root_name in SCANNED_ROOTS:
        root = REPOSITORY_ROOT / root_name
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(REPOSITORY_ROOT).as_posix()
            if any(relative.startswith(prefix) for prefix in EXCLUDED_BACKEND_PREFIXES):
                continue
            yield path, relative


def owner_for(relative):
    if relative.startswith("indra/llappearance/"):
        return "appearance_and_baking"
    if relative.startswith("indra/llrender/"):
        return "renderer_core"
    if relative.startswith("indra/llui/"):
        return "ui_core"
    if relative.startswith("indra/llwindow/"):
        return "window_and_context"

    filename = Path(relative).name.lower()
    if any(token in filename for token in (
        "snapshot", "reflection", "heroprobe", "dynamictexture", "preview",
        "impostor", "cubemap",
    )):
        return "offscreen_and_recursive"
    if any(token in filename for token in (
        "hud", "floater", "panel", "menu", "tool", "manip", "select",
        "tracker", "worldmap", "joystick", "progressview", "textureview",
    )):
        return "ui_hud_and_interaction"
    if any(token in filename for token in ("alpha", "particle", "hudeffect")):
        return "alpha_and_particles"
    if any(token in filename for token in ("terrain", "water", "wlsky", "sky")):
        return "environment"
    return "world_renderer"


def add_occurrence(table, symbol, relative):
    table[symbol][relative] += 1


def summarize_files(files):
    owners = defaultdict(int)
    for relative, count in files.items():
        owners[owner_for(relative)] += count
    return dict(sorted(owners.items()))


def ggl_classification(symbol):
    for classification, members in GGL_GROUPS.items():
        if symbol in members:
            return classification, GGL_DISPOSITIONS[classification]
    return "unclassified", "architecture_review_required"


def gl_classification(symbol):
    if symbol in PROJECT_GL_SHAPED_SYMBOLS:
        return "project_symbol_false_positive", "exclude_from_native_api_metric"
    if symbol in OBSOLETE_FIXED_FUNCTION_GL:
        return "obsolete_fixed_function", "remove_or_semantically_replace"
    if symbol in LEGACY_EXTENSION_ALIASES:
        return "legacy_extension_alias", "port_to_opengl_4_1_core_then_confine"
    if symbol in MODERN_STATE_GL:
        return "valid_modern_gl_state", "express_through_ghi_and_confine"
    return "valid_or_reviewed_native_gl", "confine_to_opengl_backend"


def build_entries(raw, classifier):
    entries = {}
    for symbol, paths in sorted(raw.items()):
        classification, disposition = classifier(symbol)
        files = dict(sorted(paths.items()))
        entries[symbol] = {
            "classification": classification,
            "disposition": disposition,
            "count": sum(files.values()),
            "owners": summarize_files(files),
            "files": files,
        }
    return entries


def collect_ledger():
    ggl = defaultdict(lambda: defaultdict(int))
    state = defaultdict(lambda: defaultdict(int))
    direct_gl = defaultdict(lambda: defaultdict(int))

    for path, relative in source_files():
        source = path.read_text(encoding="utf-8", errors="replace")
        code = COMMENT_OR_LITERAL.sub("", source)
        for match in GGL_MEMBER.finditer(code):
            add_occurrence(ggl, match.group(1), relative)
        for match in LEGACY_STATE_REFERENCE.finditer(code):
            add_occurrence(state, match.group(1), relative)
        for match in DIRECT_GL_CALL.finditer(code):
            add_occurrence(direct_gl, match.group(1), relative)

    state_entries = build_entries(state, lambda symbol: LEGACY_STATE_SYMBOLS[symbol])
    ggl_entries = build_entries(ggl, ggl_classification)
    gl_entries = build_entries(direct_gl, gl_classification)
    unclassified = [
        f"gGL.{symbol}" for symbol, entry in ggl_entries.items()
        if entry["classification"] == "unclassified"
    ]
    legacy_alias_violations = [
        f"{symbol} in {relative}"
        for symbol, paths in sorted(direct_gl.items())
        if symbol in LEGACY_EXTENSION_ALIASES
        for relative in sorted(paths)
        if relative != MACOS_GL_COMPATIBILITY_HEADER
    ]

    return {
        "schema": 1,
        "source_revision": git_revision(),
        "scope": {
            "roots": list(SCANNED_ROOTS),
            "excluded_backend_prefixes": list(EXCLUDED_BACKEND_PREFIXES),
            "note": "Static source classification; runtime reachability is a separate P0 gate.",
        },
        "summary": {
            "ggl_member_uses": sum(entry["count"] for entry in ggl_entries.values()),
            "legacy_state_symbol_uses": sum(entry["count"] for entry in state_entries.values()),
            "direct_gl_shaped_calls": sum(entry["count"] for entry in gl_entries.values()),
            "unclassified_symbols": unclassified,
            "legacy_extension_alias_violations": legacy_alias_violations,
        },
        "surfaces": {
            "ggl_members": ggl_entries,
            "legacy_state_symbols": state_entries,
            "direct_gl_calls": gl_entries,
        },
    }


def comparable(ledger):
    result = dict(ledger)
    result.pop("source_revision", None)
    return result


def main(raw_args=None):
    parser = ArgumentParser(description="Audit legacy GL state above the GHI backend boundary")
    parser.add_argument("--ledger", type=Path, default=DEFAULT_LEDGER)
    parser.add_argument("--write-ledger", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(raw_args)

    ledger = collect_ledger()
    summary = ledger["summary"]
    print("P0 legacy GL state ledger")
    print(f"  gGL member uses            {summary['ggl_member_uses']:5d}")
    print(f"  legacy state symbol uses   {summary['legacy_state_symbol_uses']:5d}")
    print(f"  direct GL-shaped calls     {summary['direct_gl_shaped_calls']:5d}")
    print(f"  unclassified symbols       {len(summary['unclassified_symbols']):5d}")
    print(f"  extension alias violations {len(summary['legacy_extension_alias_violations']):5d}")
    for symbol in summary["unclassified_symbols"]:
        print(f"    {symbol}", file=sys.stderr)
    for violation in summary["legacy_extension_alias_violations"]:
        print(f"    {violation}", file=sys.stderr)

    if args.write_ledger:
        args.ledger.parent.mkdir(parents=True, exist_ok=True)
        args.ledger.write_text(json.dumps(ledger, indent=2) + "\n", encoding="utf-8")
        print(f"Wrote {args.ledger.relative_to(REPOSITORY_ROOT)}")

    if (summary["unclassified_symbols"] or
            summary["legacy_extension_alias_violations"]):
        return 1
    if args.check:
        if not args.ledger.exists():
            print(f"Ledger does not exist: {args.ledger}", file=sys.stderr)
            return 1
        committed = json.loads(args.ledger.read_text(encoding="utf-8"))
        if comparable(committed) != comparable(ledger):
            print("P0 legacy GL state ledger is stale; regenerate and review it.", file=sys.stderr)
            return 1
        print("P0 legacy GL state ledger check passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

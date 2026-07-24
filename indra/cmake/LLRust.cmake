# -*- cmake -*-
#
# LLRust: build the `llrust` Rust crate as a static library and expose it to the
# C++ viewer as the `ll::rust` target (links libllrust.a + the cbindgen-generated
# header directory).
#
# FreeBSD-first: we build/test the Rust bridge here. Other platforms' CI has no
# cargo wired up yet, so the bridge is only defined when cargo is available; C++
# call sites guard on HAVE_LLRUST so a build without cargo still links.

include_guard()

add_library( ll::rust INTERFACE IMPORTED )

# How the Rust mesh-asset decoders (geometry, decomposition, ...) are used:
#   off        pure C++ -- no Rust built or linked
#   cfallback  Rust primary, C++ decoder kept as fallback on any Rust failure
#   on         Rust only -- the C++ decode path is compiled OUT (removes its
#              unchecked/OOB-prone parsing from the binary entirely)
# Default cfallback while the Rust path soaks in production; flip to on once it
# is bombproof. Maps to LL_RUSTMESH_MODE = 0 | 1 | 2 for the C++ #if guards.
set(LL_RUSTMESH "cfallback" CACHE STRING "Rust mesh decode: off | cfallback | on")
set_property(CACHE LL_RUSTMESH PROPERTY STRINGS off cfallback on)

if (LL_RUSTMESH STREQUAL "off")
  message(STATUS "LLRust: LL_RUSTMESH=off -> pure C++ mesh decode (no Rust)")
  return()
endif ()

if (LL_RUSTMESH STREQUAL "on")
  set(LLRUST_MODE 2)
elseif (LL_RUSTMESH STREQUAL "cfallback")
  set(LLRUST_MODE 1)
else ()
  message(FATAL_ERROR "LL_RUSTMESH must be one of: off, cfallback, on (got '${LL_RUSTMESH}')")
endif ()

find_program(CARGO_EXECUTABLE cargo)
find_program(CBINDGEN_EXECUTABLE cbindgen)

if (NOT CARGO_EXECUTABLE OR NOT CBINDGEN_EXECUTABLE)
  message(WARNING "LLRust: LL_RUSTMESH=${LL_RUSTMESH} but cargo/cbindgen not found -> falling back to pure C++")
  return()
endif ()

set(LLRUST_CRATE_DIR  "${CMAKE_SOURCE_DIR}/rust/llrust")   # indra/rust/llrust (indra = CMake source root)
set(LLRUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust")
set(LLRUST_LIB        "${LLRUST_TARGET_DIR}/release/libllrust.a")
set(LLRUST_INCLUDE    "${LLRUST_TARGET_DIR}/include")
set(LLRUST_HEADER     "${LLRUST_INCLUDE}/llrust.h")

# --- libexecinfo stub (FreeBSD 15) -------------------------------------------
# rustc's FreeBSD target spec links `-lexecinfo`, but FreeBSD 15 folded
# libexecinfo into libc and dropped the standalone library. That breaks the link
# of anything cargo builds as an *executable* -- notably dependency build scripts
# (flate2 -> crc32fast) and `cargo test` binaries -- with:
#     ld: error: unable to find library -lexecinfo
# The backtrace symbols themselves resolve fine from libc, so an *empty* archive
# is enough to satisfy the linker. Synthesize one into the build tree and put it
# on cargo's search path so a fresh checkout builds with no manual steps.
set(LLRUST_STUB_DIR "${LLRUST_TARGET_DIR}/stub")
set(LLRUST_CARGO_ENV)
# FreeBSD-only: this whole dance exists because FreeBSD 15 dropped the standalone
# libexecinfo. Elsewhere (Windows/macOS/Linux) rustc does not ask for it, and the
# cc/ar invocation below is not valid for e.g. MSVC -- so don't even look.
if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
find_library(LLRUST_LIBEXECINFO execinfo)
if (NOT LLRUST_LIBEXECINFO)
  if (NOT EXISTS "${LLRUST_STUB_DIR}/libexecinfo.a")
    message(STATUS "LLRust: libexecinfo not found (FreeBSD 15+) -> synthesizing empty stub")
    file(MAKE_DIRECTORY "${LLRUST_STUB_DIR}")
    file(WRITE "${LLRUST_STUB_DIR}/execinfo_stub.c"
         "/* Intentionally empty. Satisfies rustc's -lexecinfo on FreeBSD 15+;\n"
         "   the real backtrace symbols are provided by libc. */\n")
    set(_llrust_ar "${CMAKE_AR}")
    if (NOT _llrust_ar)
      set(_llrust_ar "ar")
    endif ()
    execute_process(
        COMMAND "${CMAKE_C_COMPILER}" -c "${LLRUST_STUB_DIR}/execinfo_stub.c"
                -o "${LLRUST_STUB_DIR}/execinfo_stub.o"
        RESULT_VARIABLE _llrust_cc_rc ERROR_VARIABLE _llrust_cc_err OUTPUT_QUIET)
    if (_llrust_cc_rc EQUAL 0)
      execute_process(
          COMMAND "${_llrust_ar}" rcs "${LLRUST_STUB_DIR}/libexecinfo.a"
                  "${LLRUST_STUB_DIR}/execinfo_stub.o"
          RESULT_VARIABLE _llrust_ar_rc ERROR_VARIABLE _llrust_ar_err OUTPUT_QUIET)
      if (NOT _llrust_ar_rc EQUAL 0)
        message(WARNING "LLRust: could not archive libexecinfo stub: ${_llrust_ar_err}")
      endif ()
    else ()
      message(WARNING "LLRust: could not compile libexecinfo stub: ${_llrust_cc_err}")
    endif ()
  endif ()
  if (EXISTS "${LLRUST_STUB_DIR}/libexecinfo.a")
    # RUSTFLAGS reaches every rustc invocation cargo makes -- including the
    # dependency build scripts that actually hit this link error.
    set(LLRUST_CARGO_ENV "${CMAKE_COMMAND}" -E env "RUSTFLAGS=-L native=${LLRUST_STUB_DIR}")
    message(STATUS "LLRust: using libexecinfo stub at ${LLRUST_STUB_DIR}")
  endif ()
endif ()
endif () # FreeBSD-only libexecinfo workaround

file(GLOB_RECURSE LLRUST_SOURCES
     "${LLRUST_CRATE_DIR}/src/*.rs"
     "${LLRUST_CRATE_DIR}/Cargo.toml"
     "${LLRUST_CRATE_DIR}/cbindgen.toml")

# Build the staticlib.
add_custom_command(
    OUTPUT "${LLRUST_LIB}"
    COMMAND ${LLRUST_CARGO_ENV} "${CARGO_EXECUTABLE}" build --release
            --manifest-path "${LLRUST_CRATE_DIR}/Cargo.toml"
            --target-dir "${LLRUST_TARGET_DIR}"
    DEPENDS ${LLRUST_SOURCES}
    WORKING_DIRECTORY "${LLRUST_CRATE_DIR}"
    COMMENT "LLRust: cargo build --release (libllrust.a)"
    VERBATIM)

# Generate the C header from the crate's extern "C" surface.
add_custom_command(
    OUTPUT "${LLRUST_HEADER}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LLRUST_INCLUDE}"
    COMMAND "${CBINDGEN_EXECUTABLE}" --config "${LLRUST_CRATE_DIR}/cbindgen.toml"
            --output "${LLRUST_HEADER}" "${LLRUST_CRATE_DIR}"
    DEPENDS ${LLRUST_SOURCES}
    COMMENT "LLRust: cbindgen -> llrust.h"
    VERBATIM)

add_custom_target(llrust_build DEPENDS "${LLRUST_LIB}" "${LLRUST_HEADER}")

# System libraries a Rust staticlib drags in (libstd's own deps). These are
# per-target and MUST come from `rustc --print native-static-libs` on that
# platform -- guessing produces link errors.
#
# Only the FreeBSD list below is verified (that is where this bridge is built
# and tested). The others are best-effort starting points for whoever enables
# the bridge there; confirm them before trusting them. Note that on Windows you
# must also match CRT linkage between Rust's MSVC target and the viewer's C++,
# and the artifact is llrust.lib rather than libllrust.a.
if (CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
  # VERIFIED. libstd touches procstat/kvm/memstat/devstat (system stats), rt
  # (clock), util, gcc_s. libc is already linked by C++. rustc also lists
  # `execinfo`, but FreeBSD 15 folded it into libc and dropped the standalone
  # library, so linking -lexecinfo fails; omit it (see the stub logic above).
  set(LLRUST_NATIVE_LIBS pthread gcc_s m rt util kvm memstat procstat devstat)
elseif (WINDOWS)
  set(LLRUST_NATIVE_LIBS ws2_32 bcrypt userenv ntdll advapi32)   # UNVERIFIED
elseif (DARWIN)
  set(LLRUST_NATIVE_LIBS System resolv c m)                      # UNVERIFIED
else ()
  set(LLRUST_NATIVE_LIBS dl rt pthread gcc_s c m util)           # UNVERIFIED (Linux)
endif ()

set_target_properties(ll::rust PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LLRUST_INCLUDE}"
    INTERFACE_LINK_LIBRARIES      "${LLRUST_LIB};${LLRUST_NATIVE_LIBS}"
    INTERFACE_COMPILE_DEFINITIONS "HAVE_LLRUST=1;LL_RUSTMESH_MODE=${LLRUST_MODE}")

# Consumers must `add_dependencies(<their_target> llrust_build)` so the .a/.h
# exist before they compile/link.
message(STATUS "LLRust: bridge enabled (${LLRUST_LIB})")

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

find_program(CARGO_EXECUTABLE cargo)
find_program(CBINDGEN_EXECUTABLE cbindgen)

if (NOT CARGO_EXECUTABLE OR NOT CBINDGEN_EXECUTABLE)
  message(STATUS "LLRust: cargo/cbindgen not found -> Rust bridge disabled (HAVE_LLRUST unset)")
  return()
endif ()

set(LLRUST_CRATE_DIR  "${CMAKE_SOURCE_DIR}/rust/llrust")   # indra/rust/llrust (indra = CMake source root)
set(LLRUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust")
set(LLRUST_LIB        "${LLRUST_TARGET_DIR}/release/libllrust.a")
set(LLRUST_INCLUDE    "${LLRUST_TARGET_DIR}/include")
set(LLRUST_HEADER     "${LLRUST_INCLUDE}/llrust.h")

file(GLOB_RECURSE LLRUST_SOURCES
     "${LLRUST_CRATE_DIR}/src/*.rs"
     "${LLRUST_CRATE_DIR}/Cargo.toml"
     "${LLRUST_CRATE_DIR}/cbindgen.toml")

# Build the staticlib.
add_custom_command(
    OUTPUT "${LLRUST_LIB}"
    COMMAND "${CARGO_EXECUTABLE}" build --release
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

# native-static-libs a Rust staticlib pulls in on FreeBSD (libstd deps), from
# `rustc --print native-static-libs`: libstd touches procstat/kvm/memstat/devstat
# (system stats), rt (clock), util, gcc_s. libc is already linked by C++.
# NOTE: rustc also lists `execinfo`, but on FreeBSD 15 libexecinfo was folded
# into libc -- the standalone lib is gone and its backtrace symbols resolve via
# libc, so linking -lexecinfo fails ("unable to find library"). Omit it.
set(LLRUST_NATIVE_LIBS pthread gcc_s m rt util kvm memstat procstat devstat)

set_target_properties(ll::rust PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LLRUST_INCLUDE}"
    INTERFACE_LINK_LIBRARIES      "${LLRUST_LIB};${LLRUST_NATIVE_LIBS}"
    INTERFACE_COMPILE_DEFINITIONS "HAVE_LLRUST=1")

# Consumers must `add_dependencies(<their_target> llrust_build)` so the .a/.h
# exist before they compile/link.
message(STATUS "LLRust: bridge enabled (${LLRUST_LIB})")

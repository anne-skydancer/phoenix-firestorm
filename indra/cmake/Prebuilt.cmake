# -*- cmake -*-
include_guard()

include(FindAutobuild)
if(INSTALL_PROPRIETARY)
  include(FindSCP)
endif(INSTALL_PROPRIETARY)

set(PREBUILD_TRACKING_DIR ${AUTOBUILD_INSTALL_DIR}/cmake_tracking)
# For the library installation process;
# see cmake/Prebuild.cmake for the counterpart code.
if ("${CMAKE_SOURCE_DIR}/../autobuild.xml" IS_NEWER_THAN "${PREBUILD_TRACKING_DIR}/sentinel_installed")
  file(MAKE_DIRECTORY ${PREBUILD_TRACKING_DIR})
  file(WRITE ${PREBUILD_TRACKING_DIR}/sentinel_installed "0")
endif ("${CMAKE_SOURCE_DIR}/../autobuild.xml" IS_NEWER_THAN "${PREBUILD_TRACKING_DIR}/sentinel_installed")

# The use_prebuilt_binary macro handles automated installation of package
# dependencies using autobuild.  The goal is that 'autobuild install' should
# only be run when we know we need to install a new package.  This should be
# the case in a clean checkout, or if autobuild.xml has been updated since the
# last run (encapsulated by the file ${PREBUILD_TRACKING_DIR}/sentinel_installed),
# or if a previous attempt to install the package has failed (the exit status
# of previous attempts is serialized in the file
# ${PREBUILD_TRACKING_DIR}/${_binary}_installed)
macro (use_prebuilt_binary _binary)
    if( NOT DEFINED ${_binary}_installed )
        set( ${_binary}_installed "")
    endif()

    if("${${_binary}_installed}" STREQUAL "" AND EXISTS "${PREBUILD_TRACKING_DIR}/${_binary}_installed")
        file(READ ${PREBUILD_TRACKING_DIR}/${_binary}_installed "${_binary}_installed")
        if(DEBUG_PREBUILT)
            message(STATUS "${_binary}_installed: \"${${_binary}_installed}\"")
        endif(DEBUG_PREBUILT)
    endif("${${_binary}_installed}" STREQUAL "" AND EXISTS "${PREBUILD_TRACKING_DIR}/${_binary}_installed")

    if(${PREBUILD_TRACKING_DIR}/sentinel_installed IS_NEWER_THAN ${PREBUILD_TRACKING_DIR}/${_binary}_installed OR NOT ${${_binary}_installed} EQUAL 0)
        if(DEBUG_PREBUILT)
            message(STATUS "cd ${CMAKE_SOURCE_DIR} && ${AUTOBUILD_EXECUTABLE} install
        --install-dir=${AUTOBUILD_INSTALL_DIR}
        ${_binary} ")
        endif(DEBUG_PREBUILT)
        execute_process(COMMAND "${AUTOBUILD_EXECUTABLE}"
                install
                --install-dir=${AUTOBUILD_INSTALL_DIR}
                ${_binary}
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
                RESULT_VARIABLE ${_binary}_installed
                )
        file(WRITE ${PREBUILD_TRACKING_DIR}/${_binary}_installed "${${_binary}_installed}")
    endif(${PREBUILD_TRACKING_DIR}/sentinel_installed IS_NEWER_THAN ${PREBUILD_TRACKING_DIR}/${_binary}_installed OR NOT ${${_binary}_installed} EQUAL 0)

    if(NOT ${_binary}_installed EQUAL 0)
        message(FATAL_ERROR
                "Failed to download or unpack prebuilt '${_binary}'."
                " Process returned ${${_binary}_installed}.")
    endif (NOT ${_binary}_installed EQUAL 0)
endmacro (use_prebuilt_binary _binary)

# ---------------------------------------------------------------------------
# System-library resolution.
#
# Used on platforms that have no autobuild prebuilts (e.g. FreeBSD), where
# dependencies come from the OS package manager instead. Gated by USESYSTEMLIBS:
#   OFF (default on Windows/macOS/Linux) -> use_system_binary() is a no-op and
#       the caller falls through to use_prebuilt_binary(); those builds are
#       completely unaffected.
#   ON  (default on FreeBSD)             -> resolve ll::<name> from a system
#       library and return(), short-circuiting the prebuilt tail of the file.
if (NOT DEFINED USESYSTEMLIBS)
  if (CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
    set(USESYSTEMLIBS ON  CACHE BOOL "Resolve deps from system packages, not autobuild prebuilts")
  else ()
    set(USESYSTEMLIBS OFF CACHE BOOL "Resolve deps from system packages, not autobuild prebuilts")
  endif ()
endif ()

# Manifest: ll:: dependency name -> the pkg-config module(s) that actually
# provide it. Only names whose .pc module differs (or need extra modules) go
# here; anything absent falls back to trying "<name>" then "lib<name>".
# Values verified against FreeBSD 15 pkg; revisit for other distros.
set(SYSLIB_MODULE_freetype  freetype2)
set(SYSLIB_MODULE_apr       apr-1)
set(SYSLIB_MODULE_apr-util  apr-util-1)
set(SYSLIB_MODULE_nghttp2   libnghttp2)
set(SYSLIB_MODULE_libpng    libpng16)
set(SYSLIB_MODULE_SDL2      sdl2)
set(SYSLIB_MODULE_dbus      dbus-1)
set(SYSLIB_MODULE_openjpeg  libopenjp2)

# Sadly this must be a macro: the return() has to fire in the *caller* so the
# prebuilt tail of the including file is skipped.
#
# use_system_binary(<name> [<extra>...])
#   Resolves ll::<name>. Extra args are additional deps to link into the same
#   target (e.g. `use_system_binary(apr apr-util)` -> apr-1 + apr-util-1).
macro (use_system_binary name)
  if (USE_CONAN)
    target_link_libraries(ll::${name} INTERFACE CONAN_PKG::${name})
    foreach (extra_pkg ${ARGN})
      if (extra_pkg)
        target_link_libraries(ll::${name} INTERFACE CONAN_PKG::${extra_pkg})
      endif ()
    endforeach ()
    return()
  endif ()

  if (USESYSTEMLIBS)
    include(FindPkgConfig)

    # Translate each name through the manifest into real pkg-config modules.
    set(_usb_mods "")
    foreach (_a ${name} ${ARGN})
      if (DEFINED SYSLIB_MODULE_${_a})
        list(APPEND _usb_mods ${SYSLIB_MODULE_${_a}})
      else ()
        list(APPEND _usb_mods ${_a})
      endif ()
    endforeach ()

    pkg_check_modules(SYS_${name} QUIET IMPORTED_TARGET ${_usb_mods})
    if (SYS_${name}_FOUND)
      target_link_libraries(ll::${name} INTERFACE PkgConfig::SYS_${name})
      message(STATUS "ll::${name}: system pkg-config [${_usb_mods}] ${SYS_${name}_VERSION}")
      return()
    endif ()

    # cmake-config libraries (glm, meshoptimizer, ...) ship no .pc file.
    find_package(${name} QUIET)
    if (${name}_FOUND)
      target_include_directories(ll::${name} SYSTEM INTERFACE ${${name}_INCLUDE_DIRS})
      target_link_libraries(ll::${name} INTERFACE ${${name}_LIBRARIES})
      message(STATUS "ll::${name}: system find_package")
      return()
    endif ()

    message(FATAL_ERROR
      "use_system_binary(${name}): no system library for pkg-config module(s) "
      "'${_usb_mods}', and find_package(${name}) failed. Install the OS package "
      "that provides it, or add a SYSLIB_MODULE_${name} mapping / vendored arm "
      "in this dep's cmake file.")
  endif ()

  # USESYSTEMLIBS OFF: intentional no-op; caller proceeds to use_prebuilt_binary().
endmacro ()


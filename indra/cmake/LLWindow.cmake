# -*- cmake -*-

include(Variables)
include(GLEXT)
include(Prebuilt)

include_guard()
add_library( ll::SDL INTERFACE IMPORTED )

if (FREEBSD)
  # FreeBSD pulls SDL2 + X11 straight from pkg (system libraries) rather than the
  # autobuild-shaped use_system_binary/use_prebuilt_binary path used for Linux.
  include(FindPkgConfig)
  pkg_check_modules(Sdl2 REQUIRED sdl2)
  target_compile_definitions( ll::SDL INTERFACE LL_SDL2=1 LL_SDL=1 )
  target_include_directories( ll::SDL SYSTEM INTERFACE ${Sdl2_INCLUDE_DIRS} )
  target_link_directories( ll::SDL INTERFACE ${Sdl2_LIBRARY_DIRS} )
  target_link_libraries( ll::SDL INTERFACE ${Sdl2_LIBRARIES} X11 )
  return()
endif ()

if (LINUX)
  #Must come first as use_system_binary can exit this file early
  #target_compile_definitions( ll::SDL INTERFACE LL_SDL=1)

  #use_system_binary(SDL)
  #use_prebuilt_binary(SDL)
  
  target_include_directories( ll::SDL SYSTEM INTERFACE ${LIBS_PREBUILT_DIR}/include)

  if( USE_SDL1 )
    target_compile_definitions( ll::SDL INTERFACE LL_SDL=1 )

    use_system_binary(SDL)
    use_prebuilt_binary(SDL)
    set (SDL_FOUND TRUE)

    target_link_libraries (ll::SDL INTERFACE SDL directfb fusion direct X11)

  else()
    target_compile_definitions( ll::SDL INTERFACE LL_SDL2=1 LL_SDL=1 )

    use_system_binary(SDL2)
    use_prebuilt_binary(SDL2)
    set (SDL2_FOUND TRUE)

    target_link_libraries( ll::SDL INTERFACE SDL2 X11 )
  endif()
endif (LINUX)



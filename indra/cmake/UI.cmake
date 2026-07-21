# -*- cmake -*-
include(Prebuilt)
include(FreeType)
include(GLIB)

add_library( ll::uilibraries INTERFACE IMPORTED )

if (LINUX)
  use_prebuilt_binary(fltk)
  target_compile_definitions(ll::uilibraries INTERFACE LL_FLTK=1 LL_X11=1 )

  if( USE_CONAN )
    target_link_libraries( ll::uilibraries INTERFACE CONAN_PKG::gtk )
    return()
  endif()
  
  target_link_libraries( ll::uilibraries INTERFACE
          fltk
          X11
          Xinerama
          glib-2.0
          gmodule-2.0
          gobject-2.0
          gthread-2.0
          Xinerama
          ll::freetype
          )
    target_include_directories( ll::uilibraries SYSTEM INTERFACE
            ${GLIB_INCLUDE_DIRS}
            )
endif (LINUX)
if (FREEBSD)
  # Mirror the Linux UI stack from OS packages: fltk (native file dialogs),
  # X11/Xinerama, and glib. LL_X11 gates the X11 clipboard/display code in
  # llwindowsdl2; LL_FLTK gates the fltk file/dir pickers.
  include(FindPkgConfig)
  pkg_check_modules(Glib REQUIRED glib-2.0 gmodule-2.0 gobject-2.0 gthread-2.0)
  target_compile_definitions(ll::uilibraries INTERFACE LL_FLTK=1 LL_X11=1 )
  target_include_directories( ll::uilibraries SYSTEM INTERFACE ${Glib_INCLUDE_DIRS} )
  target_link_libraries( ll::uilibraries INTERFACE
          fltk
          X11
          Xinerama
          ${Glib_LIBRARIES}
          ll::freetype
          )
endif (FREEBSD)
if( WINDOWS )
  target_link_libraries( ll::uilibraries INTERFACE
          opengl32
          comdlg32
          dxguid
          kernel32
          odbc32
          odbccp32
          oleaut32
          shell32
          Vfw32
          wer
          winspool
          imm32
          )
endif()

target_include_directories( ll::uilibraries SYSTEM INTERFACE
        ${LIBS_PREBUILT_DIR}/include
        )


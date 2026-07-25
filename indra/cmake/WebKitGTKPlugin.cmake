# -*- cmake -*-
include(Linking)

include_guard()
add_library( ll::webkitgtk INTERFACE IMPORTED )

# WebKit2GTK MOAP plugin: FreeBSD only. Uses the webkit2-gtk_40 pkg (pkg-config
# module "webkit2gtk-4.0", the libsoup-2.4 API variant of WebKit 2.46, GTK3) to
# provide web + image media rendering in place of CEF/dullahan (unavailable on
# FreeBSD). NOTE: webkit2gtk-4.1 (libsoup-3.0) is deliberately NOT used -- its
# .pc chain requires krb5-gssapi.pc, which FreeBSD's base Heimdal doesn't ship,
# so pkg-config resolution fails. 4.0 pulls libsoup-2.4 and resolves cleanly.
if (FREEBSD)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET webkit2gtk-4.0 gtk+-3.0)

  set(WEBKITGTKPLUGIN ON CACHE BOOL "WebKit2GTK MOAP plugin support.")

  target_link_libraries( ll::webkitgtk INTERFACE PkgConfig::WEBKITGTK )
  target_include_directories( ll::webkitgtk SYSTEM INTERFACE ${WEBKITGTK_INCLUDE_DIRS} )
  return()
endif ()

# No other platform builds this plugin (Windows/macOS/Linux use CEF).
message(STATUS "WebKitGTKPlugin: not building (non-FreeBSD platform)")

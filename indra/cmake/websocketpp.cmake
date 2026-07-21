# -*- cmake -*-
include(Prebuilt)

add_library( ll::websocketpp INTERFACE IMPORTED )

# SLua: the WS files include boost_config.hpp, which selects Boost type-traits, but
# websocketpp only pulls <boost/aligned_storage.hpp> (not is_same) -> "boost::is_same
# not found". Force C++11 std type-traits instead (viewer builds as C++17).
target_compile_definitions( ll::websocketpp INTERFACE _WEBSOCKETPP_CPP11_TYPE_TRAITS_ )

if (NOT FREEBSD)
  use_system_binary( websocketpp )
  use_prebuilt_binary(websocketpp)
endif ()
# FreeBSD: header-only, from the system include path (pkg install websocketpp).

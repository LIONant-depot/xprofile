include(${CMAKE_BINARY_DIR}/_deps/xcmake_tools/Common.cmake)

# xprofile: the profiler client of the whole process. Several depots fetch it (each into its own binary folder): the target is made once.
if(TARGET xprofile)
  return()
endif()

# Tracy (BSD-3): its client is compiled into xprofile's shared library; its version comes from the host's pins file (third_party.txt)
FetchAndPopulate("https://github.com/wolfpld/tracy.git" "v0.14.1")

option(XLION_PROFILE "The profiler compiled in (xprofile: Tracy's client, off until enabled at run time); OFF: the macros are nothing" ON)

set(XPROFILE_ROOT "${CMAKE_SOURCE_DIR}/dependencies/xprofile")
set(XPROFILE_TRACY "${CMAKE_SOURCE_DIR}/dependencies/tracy")

if(XLION_PROFILE)
  # one shared library: one client, one switch, whatever module (a Level's own copy of the engine too) the zones come from
  add_library(xprofile SHARED
    "${XPROFILE_ROOT}/source/xprofile.h"
    "${XPROFILE_ROOT}/source/xprofile.cpp"
    "${XPROFILE_TRACY}/public/TracyClient.cpp"
  )
  target_include_directories(xprofile PUBLIC "${XPROFILE_ROOT}/source" "${XPROFILE_TRACY}/public")
  # TRACY_ON_DEMAND: nothing is kept until a viewer connects (no memory growing with nobody watching); MANUAL_LIFETIME + DELAYED_INIT:
  # the client starts when the profiler is enabled, not when the library loads
  target_compile_definitions(xprofile
    PUBLIC    XLION_PROFILE=1 TRACY_ENABLE TRACY_ON_DEMAND TRACY_MANUAL_LIFETIME TRACY_DELAYED_INIT
    PRIVATE   XPROFILE_EXPORTS TRACY_EXPORTS
    INTERFACE TRACY_IMPORTS)
  if(WIN32)
    target_link_libraries(xprofile PRIVATE ws2_32 dbghelp)
  else()
    target_link_libraries(xprofile PRIVATE pthread dl)
  endif()
else()
  add_library(xprofile INTERFACE)
  target_include_directories(xprofile INTERFACE "${XPROFILE_ROOT}/source")
endif()

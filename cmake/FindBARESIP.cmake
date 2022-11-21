find_package(PkgConfig QUIET)
pkg_check_modules(PC_BARESIP QUIET libbaresip)

find_path(BARESIP_INCLUDE_DIR baresip.h
  HINTS ../baresip/include ${PC_BARESIP_INCLUDEDIR} ${PC_BARESIP_INCLUDE_DIRS})

find_library(BARESIP_LIBRARY NAMES baresip libbaresip baresip-static
  HINTS ../baresip ../baresip/build ../baresip/build/Debug
  ${PC_BARESIP_LIBDIR} ${PC_BARESIP_LIBRARY_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(BARESIP DEFAULT_MSG BARESIP_LIBRARY BARESIP_INCLUDE_DIR)

mark_as_advanced(BARESIP_INCLUDE_DIR BARESIP_LIBRARY)

set(BARESIP_INCLUDE_DIRS ${BARESIP_INCLUDE_DIR})
set(BARESIP_LIBRARIES ${BARESIP_LIBRARY})

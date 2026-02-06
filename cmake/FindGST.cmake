find_package(PkgConfig REQUIRED)

# --- GStreamer ---
pkg_check_modules(GST
  gstreamer-1.0
  gstreamer-base-1.0
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(GST DEFAULT_MSG
  GST_INCLUDE_DIRS
  GST_LIBRARIES
  GST_LIBRARY_DIRS
)

mark_as_advanced(
  GST_INCLUDE_DIRS
  GST_LIBRARIES
  GST_LIBRARY_DIRS
)

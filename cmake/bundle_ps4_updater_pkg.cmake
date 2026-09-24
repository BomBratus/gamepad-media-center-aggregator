# Bundle the PS4 updater-helper PKG into GMCA's package staging directory.
# This runs at build time because the helper PKG does not exist at configure time.
file(GLOB_RECURSE _pkgs "${SRC_DIR}/*GMCA00002*.pkg")
list(LENGTH _pkgs _n)
if(_n EQUAL 0)
    message(FATAL_ERROR "bundle_ps4_updater_pkg: no GMCA00002 pkg found under ${SRC_DIR}")
endif()
list(GET _pkgs 0 _pkg)
message(STATUS "bundle_ps4_updater_pkg: ${_pkg} -> ${DST}")
configure_file("${_pkg}" "${DST}" COPYONLY)

# Resolve the upstream recipe's relative PATCHES against its pinned port directory.
# Only Android needs PIC.
if(VCPKG_TARGET_IS_ANDROID)
    list(APPEND VCPKG_MAKE_CONFIGURE_OPTIONS --with-pic)
endif()

set(_MC_MPFR_OVERLAY_PORT_DIR "${CURRENT_PORT_DIR}")
set(CURRENT_PORT_DIR "${VCPKG_ROOT_DIR}/ports/mpfr")
include("${VCPKG_ROOT_DIR}/ports/mpfr/portfile.cmake")
set(CURRENT_PORT_DIR "${_MC_MPFR_OVERLAY_PORT_DIR}")
unset(_MC_MPFR_OVERLAY_PORT_DIR)

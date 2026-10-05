# Keep the pinned MPFR portfile and its patches; only Android needs PIC here.
if(VCPKG_TARGET_IS_ANDROID)
    list(APPEND VCPKG_MAKE_CONFIGURE_OPTIONS --with-pic)
endif()

include("${VCPKG_ROOT_DIR}/ports/mpfr/portfile.cmake")

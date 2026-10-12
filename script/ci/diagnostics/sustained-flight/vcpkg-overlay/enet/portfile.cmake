vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH
    REPO  "lsalzman/enet"
    REF "v${VERSION}"
    HEAD_REF master
    SHA512 a0d2fa8c957704dd49e00a726284ac5ca034b50b00d2b20a94fa1bbfbb80841467834bfdc84aa0ed0d6aab894608fd6c86c3b94eee46343f0e6d9c22e391dbf9
    PATCHES
        "${CMAKE_CURRENT_LIST_DIR}/fix-export.patch"
        "${CMAKE_CURRENT_LIST_DIR}/enet-rejection-probe.patch"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-enet CONFIG_PATH share/unofficial-enet)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_copy_pdbs()

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

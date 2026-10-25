cmake_minimum_required(VERSION 3.25)

set(HAVE_COLAMD 1)
set(XSDK_INDEX_SIZE "")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/SRC/superlu_config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/superlu_config.h"
)

add_library(mc_v9_superlu STATIC
    SRC/superlu_timer.c
    SRC/util.c
    SRC/memory.c
    SRC/get_perm_c.c
    SRC/mmd.c
    SRC/sp_coletree.c
    SRC/sp_preorder.c
    SRC/sp_ienv.c
    SRC/relax_snode.c
    SRC/heap_relax_snode.c
    SRC/colamd.c
    SRC/mark_relax.c
    SRC/qselect.c
    SRC/input_error.c
    SRC/dlacon2.c
    SRC/dmach.c
    SRC/dgssv.c
    SRC/dsp_blas2.c
    SRC/dsp_blas3.c
    SRC/dgstrf.c
    SRC/dgstrs.c
    SRC/dcopy_to_ucol.c
    SRC/dsnode_dfs.c
    SRC/dsnode_bmod.c
    SRC/dpanel_dfs.c
    SRC/dpanel_bmod.c
    SRC/dcolumn_dfs.c
    SRC/dcolumn_bmod.c
    SRC/dpivotL.c
    SRC/dpruneL.c
    SRC/dmemory.c
    SRC/dutil.c
    SRC/dmyblas2.c
    CBLAS/dtrsv.c
)

target_include_directories(mc_v9_superlu
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/SRC"
        "${CMAKE_CURRENT_SOURCE_DIR}/CBLAS"
        "${CMAKE_CURRENT_BINARY_DIR}"
)
set_target_properties(mc_v9_superlu PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
if(NOT WIN32)
    target_link_libraries(mc_v9_superlu PUBLIC m)
endif()

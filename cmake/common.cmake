# common.cmake
# Common helper functions

function(sb_collect_sources root out_sources out_headers)
  file(GLOB_RECURSE _all_cpp LIST_DIRECTORIES FALSE
       "${root}/*.cpp" "${root}/*.cxx" "${root}/*.cc")
  file(GLOB_RECURSE _all_h LIST_DIRECTORIES FALSE
       "${root}/*.h" "${root}/*.hpp" "${root}/*.inl")

  set(_exclude_substrings
    "/Tests/"
    "/Tests\\"
    "/vcpkg_installed/"
    "/vcpkg_installed\\"
    "/.vcpkg/"
    "/.vcpkg\\"
    "/installed/"
    "/installed\\"
    "/build/"
    "/build\\"
    "/third_party/"
    "/third_party\\"
    "/external/"
    "/external\\"
  )

  set(_cpps "")
  foreach(f IN LISTS _all_cpp)
    set(_skip FALSE)
    foreach(ex IN LISTS _exclude_substrings)
      string(FIND "${f}" "${ex}" _idx)
      if(NOT _idx EQUAL -1)
        set(_skip TRUE)
        break()
      endif()
    endforeach()
    if(NOT _skip)
      list(APPEND _cpps "${f}")
    endif()
  endforeach()
  set(${out_sources} "${_cpps}" PARENT_SCOPE)

  set(_hs "")
  foreach(f IN LISTS _all_h)
    set(_skip FALSE)
    foreach(ex IN LISTS _exclude_substrings)
      string(FIND "${f}" "${ex}" _idx)
      if(NOT _idx EQUAL -1)
        set(_skip TRUE)
        break()
      endif()
    endforeach()
    if(NOT _skip)
      list(APPEND _hs "${f}")
    endif()
  endforeach()
  set(${out_headers} "${_hs}" PARENT_SCOPE)
endfunction()

function(sb_collect_public_headers root out_public_headers)
  file(GLOB_RECURSE _pub_h LIST_DIRECTORIES FALSE
       "${root}/*/Public/*.h" "${root}/*/Public/*.hpp"
       "${root}/*/Public/**/*.h" "${root}/*/Public/**/*.hpp")
  set(${out_public_headers} "${_pub_h}" PARENT_SCOPE)
endfunction()

function(sb_generate_public_headers_mirror target include_dir)
    if (NOT TARGET ${target})
        message(FATAL_ERROR "Target ${target} does not exist")
    endif()

    file(MAKE_DIRECTORY "${include_dir}/${target}")

    get_target_property(srcs ${target} SOURCES)
    foreach(src IN LISTS srcs)
        if (src MATCHES ".*/Public/(.+)$")
            set(rel_path "${CMAKE_MATCH_1}")
            set(dst "${include_dir}/${target}/${rel_path}")
            get_filename_component(dst_dir "${dst}" DIRECTORY)
            file(MAKE_DIRECTORY "${dst_dir}")
            configure_file("${src}" "${dst}" COPYONLY)
        endif()
    endforeach()
endfunction()

function(sb_setup_target_common tgt)
  if(MSVC)
    target_compile_options(${tgt} PRIVATE /W4 /permissive-)
    target_compile_definitions(${tgt} PRIVATE NOMINMAX)
  else()
    target_compile_options(${tgt} PRIVATE -Wall -Wextra -Wpedantic)
  endif()

  if(SHADBERRY_ARCH_AUTO)
    if(MSVC)
      target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:/arch:AVX2>)
    else()
      target_compile_options(${tgt} PRIVATE $<$<CONFIG:Release>:-march=native>)
    endif()
  endif()

  if(MSVC)
    target_compile_options(${tgt} PRIVATE
      $<$<CONFIG:Release>:/O2>
      $<$<CONFIG:RelWithDebInfo>:/O2 /Zi>
      $<$<CONFIG:Debug>:/Zi /Od>
    )
    set_property(TARGET ${tgt} PROPERTY CXX_STANDARD 23)
    if(SHADBERRY_ENABLE_IPO)
      set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endif()
  else()
    target_compile_options(${tgt} PRIVATE
      $<$<CONFIG:Release>:-O3 -DNDEBUG>
      $<$<CONFIG:RelWithDebInfo>:-O2 -g>
      $<$<CONFIG:Debug>:-O0 -g>
    )
    if(SHADBERRY_ENABLE_IPO)
      set_property(TARGET ${tgt} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
  endif()
endfunction()

function(sb_create_module_library name src_root)
  sb_collect_sources(${src_root} _sources _headers)
  sb_generate_public_headers_mirror(${CMAKE_SOURCE_DIR}/Shadberry _public_mirror)

  if(SHADBERRY_BUILD_SHARED)
    add_library(${name} SHARED ${_sources} ${_headers})
  else()
    add_library(${name} STATIC ${_sources} ${_headers})
  endif()

  target_include_directories(${name}
    PRIVATE
      ${src_root}
    INTERFACE
      $<BUILD_INTERFACE:${_public_mirror}>
  )

  sb_setup_target_common(${name})
  add_library(${name}::${name} ALIAS ${name})
endfunction()

function(sb_add_tests test_root lib_target)
  file(GLOB_RECURSE _test_sources LIST_DIRECTORIES FALSE "${test_root}/Tests/*.cpp" "${test_root}/Tests/*.cc" "${test_root}/Tests/*.cxx")
  if(NOT _test_sources)
    return()
  endif()

  find_package(GTest CONFIG REQUIRED)
  enable_testing()
  foreach(tsrc IN LISTS _test_sources)
    get_filename_component(tname "${tsrc}" NAME_WE)
    set(test_target "test_${tname}")
    add_executable(${test_target} ${tsrc})
    target_link_libraries(${test_target} PRIVATE GTest::gtest_main ${lib_target})
    sb_setup_target_common(${test_target})
    add_test(NAME ${test_target} COMMAND ${test_target})
  endforeach()
endfunction()

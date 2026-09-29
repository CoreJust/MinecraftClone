include_guard(GLOBAL)

function(mc_enable_sanitizers target)
    if(NOT MC_ENABLE_SANITIZERS)
        return()
    endif()

    # Keep MC_ENABLE_SANITIZERS as the compatibility switch used by all
    # existing presets.  MC_SANITIZER selects the instrumentation set for
    # analysis builds; the historical default remains ASan+UBSan.
    set(MC_SANITIZER "address,undefined" CACHE STRING
        "Sanitizer set: address,undefined, address, thread, hwaddress, leak, or memory"
    )

    if(MSVC)
        if(
            MC_SANITIZER MATCHES "(^|,)address(,|$)"
            AND NOT MC_SANITIZER MATCHES "(^|,)(thread|memory|hwaddress)(,|$)"
        )
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
        else()
            message(WARNING
                "MC_ENABLE_SANITIZERS requested unsupported MSVC sanitizer set '${MC_SANITIZER}'; "
                "only address is available"
            )
        endif()
        return()
    endif()

    if(
        NOT MC_SANITIZER MATCHES
        "^(address|undefined|thread|hwaddress|leak|memory)(,(address|undefined|thread|hwaddress|leak|memory))*$"
    )
        message(FATAL_ERROR "Unsupported MC_SANITIZER value: ${MC_SANITIZER}")
    endif()

    if(MC_SANITIZER MATCHES "(^|,)(leak|memory)(,|$)")
        if(
            NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
            OR NOT MC_ANALYSIS_HOST
            OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"
        )
            message(FATAL_ERROR "LeakSanitizer and MemorySanitizer are enabled only for the Clang Linux analysis host")
        endif()
    endif()

    target_compile_options(${target} PRIVATE -fsanitize=${MC_SANITIZER} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=${MC_SANITIZER})
    if(MC_SANITIZER MATCHES "(^|,)memory(,|$)")
        target_compile_options(${target} PRIVATE -fsanitize-memory-track-origins=2)
    endif()
endfunction()

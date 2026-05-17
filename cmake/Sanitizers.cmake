include_guard(GLOBAL)

function(mc_enable_sanitizers target)
    if(NOT MC_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        message(WARNING "Sanitizers are not configured for MSVC in this starter scaffold.")
        return()
    endif()

    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()

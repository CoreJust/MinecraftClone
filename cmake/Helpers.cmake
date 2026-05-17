include_guard(GLOBAL)

include(Warnings)
include(Sanitizers)

function(mc_apply_target_defaults target)
    target_compile_features(${target} PRIVATE cxx_std_23)
    mc_enable_warnings(${target})
    mc_enable_sanitizers(${target})
    set_target_properties(${target}
        PROPERTIES
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN YES
    )
endfunction()

target_sources(mc
    PRIVATE
        EvidenceJson.cpp
        ImageEvidence.cpp
        RenderCapture.cpp
        RendererBenchmark.cpp
        ScenarioRunner.cpp
    PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/include
        FILES
            include/acceptance/EvidenceJson.hpp
            include/acceptance/ImageEvidence.hpp
            include/acceptance/RenderCapture.hpp
            include/acceptance/RendererBenchmark.hpp
            include/acceptance/ScenarioRunner.hpp
)

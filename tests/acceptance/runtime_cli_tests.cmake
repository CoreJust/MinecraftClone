if(NOT DEFINED MC_MAIN OR NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "MC_MAIN and OUTPUT_DIRECTORY are required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")

foreach(launch_mode IN ITEMS --server --local-game)
    execute_process(
        COMMAND "${MC_MAIN}" "${launch_mode}" --corelang-backend invalid --port 0
        RESULT_VARIABLE invalid_backend_result
        OUTPUT_VARIABLE invalid_backend_stdout
        ERROR_VARIABLE invalid_backend_stderr
        TIMEOUT 10
    )
    if(invalid_backend_result EQUAL 0
        OR NOT "${invalid_backend_stdout}${invalid_backend_stderr}"
            MATCHES "CoreLang backend must be interpreter, jit, or aot")
        message(FATAL_ERROR "${launch_mode} did not reject an invalid CoreLang backend")
    endif()

    execute_process(
        COMMAND "${MC_MAIN}" "${launch_mode}" --port 0
        RESULT_VARIABLE default_backend_result
        OUTPUT_VARIABLE default_backend_stdout
        ERROR_VARIABLE default_backend_stderr
        TIMEOUT 10
    )
    if(default_backend_result EQUAL 0
        OR NOT "${default_backend_stdout}${default_backend_stderr}" MATCHES "invalid port")
        message(FATAL_ERROR "${launch_mode} did not retain the default backend launch path")
    endif()

    foreach(backend IN ITEMS interpreter jit aot)
        execute_process(
            COMMAND "${MC_MAIN}" "${launch_mode}" --corelang-backend "${backend}" --port 0
            RESULT_VARIABLE selected_backend_result
            OUTPUT_VARIABLE selected_backend_stdout
            ERROR_VARIABLE selected_backend_stderr
            TIMEOUT 10
        )
        if(selected_backend_result EQUAL 0
            OR NOT "${selected_backend_stdout}${selected_backend_stderr}" MATCHES "invalid port")
            message(FATAL_ERROR "${launch_mode} did not accept CoreLang backend ${backend}")
        endif()
    endforeach()
endforeach()

execute_process(
    COMMAND "${MC_MAIN}" --benchmark-game --corelang-backend invalid
        --evidence "${OUTPUT_DIRECTORY}/invalid-backend.json"
    RESULT_VARIABLE invalid_benchmark_backend_result
    OUTPUT_VARIABLE invalid_benchmark_backend_stdout
    ERROR_VARIABLE invalid_benchmark_backend_stderr
    TIMEOUT 10
)
if(invalid_benchmark_backend_result EQUAL 0
    OR NOT "${invalid_benchmark_backend_stdout}${invalid_benchmark_backend_stderr}"
        MATCHES "CoreLang backend must be interpreter, jit, or aot")
    message(FATAL_ERROR "game benchmark did not reject an invalid CoreLang backend")
endif()

execute_process(
    COMMAND "${MC_MAIN}" --benchmark-game --terrain-model invalid
        --evidence "${OUTPUT_DIRECTORY}/invalid-terrain-model.json"
    RESULT_VARIABLE invalid_terrain_model_result
    OUTPUT_VARIABLE invalid_terrain_model_stdout
    ERROR_VARIABLE invalid_terrain_model_stderr
    TIMEOUT 10
)
if(invalid_terrain_model_result EQUAL 0
    OR NOT "${invalid_terrain_model_stdout}${invalid_terrain_model_stderr}"
        MATCHES "terrain model must be r6 or v9")
    message(FATAL_ERROR "game benchmark did not reject an invalid terrain model")
endif()

execute_process(
    COMMAND "${MC_MAIN}" --benchmark-game --terrain-model v9
        --workload ordinary-turning --gameplay-proof performance
        --evidence "${OUTPUT_DIRECTORY}/v9-gameplay-proof.json"
    RESULT_VARIABLE v9_gameplay_proof_result
    OUTPUT_VARIABLE v9_gameplay_proof_stdout
    ERROR_VARIABLE v9_gameplay_proof_stderr
    TIMEOUT 10
)
if(v9_gameplay_proof_result EQUAL 0
    OR NOT "${v9_gameplay_proof_stdout}${v9_gameplay_proof_stderr}"
        MATCHES "gameplay proof requires the R6 terrain model")
    message(FATAL_ERROR "game benchmark did not keep gameplay proof bound to R6")
endif()

foreach(backend IN ITEMS interpreter jit aot)
    execute_process(
        COMMAND "${MC_MAIN}" --benchmark-game --workload ordinary-turning
            --gameplay-proof performance --render-distance 128 --corelang-backend "${backend}"
            --evidence "${OUTPUT_DIRECTORY}/backend-${backend}.json"
        RESULT_VARIABLE selected_benchmark_backend_result
        OUTPUT_VARIABLE selected_benchmark_backend_stdout
        ERROR_VARIABLE selected_benchmark_backend_stderr
        TIMEOUT 10
    )
    if(selected_benchmark_backend_result EQUAL 0
        OR NOT "${selected_benchmark_backend_stdout}${selected_benchmark_backend_stderr}"
            MATCHES "gameplay proof conflicts with render distance")
        message(FATAL_ERROR "game benchmark did not accept CoreLang backend ${backend}")
    endif()
endforeach()

execute_process(
    COMMAND "${MC_MAIN}" --benchmark-game --workload ordinary-turning
        --gameplay-proof performance --render-distance 128
        --evidence "${OUTPUT_DIRECTORY}/backend-default.json"
    RESULT_VARIABLE default_benchmark_backend_result
    OUTPUT_VARIABLE default_benchmark_backend_stdout
    ERROR_VARIABLE default_benchmark_backend_stderr
    TIMEOUT 10
)
if(default_benchmark_backend_result EQUAL 0
    OR NOT "${default_benchmark_backend_stdout}${default_benchmark_backend_stderr}"
        MATCHES "gameplay proof conflicts with render distance")
    message(FATAL_ERROR "game benchmark did not retain the default backend parser path")
endif()

execute_process(
    COMMAND "${MC_MAIN}" --local-game --port 0
    RESULT_VARIABLE invalid_local_port_result
    OUTPUT_VARIABLE invalid_local_port_stdout
    ERROR_VARIABLE invalid_local_port_stderr
    TIMEOUT 10
)
if(invalid_local_port_result EQUAL 0
    OR NOT "${invalid_local_port_stdout}${invalid_local_port_stderr}" MATCHES "invalid port")
    message(FATAL_ERROR "Local game did not reject its invalid port before opening a window")
endif()

foreach(invalid_seconds IN ITEMS 0 121 bad)
    execute_process(
        COMMAND "${MC_MAIN}" --player-client-2d --address 127.0.0.1:20040
            --replay stationary --seconds "${invalid_seconds}" --evidence "${OUTPUT_DIRECTORY}/map"
        RESULT_VARIABLE invalid_map_result
        OUTPUT_VARIABLE invalid_map_stdout
        ERROR_VARIABLE invalid_map_stderr
        TIMEOUT 10
    )
    if(invalid_map_result EQUAL 0
        OR NOT "${invalid_map_stdout}${invalid_map_stderr}" MATCHES "2D replay seconds must be")
        message(FATAL_ERROR "2D replay accepted an invalid duration: ${invalid_seconds}")
    endif()
endforeach()

foreach(proof IN ITEMS terrain-coverage performance)
    execute_process(
        COMMAND "${MC_MAIN}" --benchmark-game --workload speed-200-full-radius
            --gameplay-proof "${proof}" --render-distance 7
            --evidence "${OUTPUT_DIRECTORY}/invalid-proof.json"
        RESULT_VARIABLE invalid_proof_result
        OUTPUT_VARIABLE invalid_proof_stdout
        ERROR_VARIABLE invalid_proof_stderr
        TIMEOUT 10
    )
    if(invalid_proof_result EQUAL 0 OR NOT "${invalid_proof_stdout}${invalid_proof_stderr}" MATCHES "conflicts with render distance")
        message(FATAL_ERROR "Game benchmark accepted an incorrect ${proof} radius: ${invalid_proof_stdout}${invalid_proof_stderr}")
    endif()
endforeach()

foreach(radius_case IN ITEMS explicit-256 implicit-default)
    set(radius_arguments)
    if(radius_case STREQUAL "explicit-256")
        list(APPEND radius_arguments --render-distance 256)
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env MC_DIAGNOSTIC_PRESENTATION_STAGES=1 "${MC_MAIN}"
            --benchmark-game --workload ordinary-turning --gameplay-proof performance
            ${radius_arguments} --evidence "${OUTPUT_DIRECTORY}/valid-performance-${radius_case}.json"
        RESULT_VARIABLE performance_proof_result
        OUTPUT_VARIABLE performance_proof_stdout
        ERROR_VARIABLE performance_proof_stderr
        TIMEOUT 10
    )
    if(performance_proof_result EQUAL 0
        OR NOT "${performance_proof_stdout}${performance_proof_stderr}"
            MATCHES "gameplay proof requires its fixed radius and production settings")
        message(FATAL_ERROR
            "performance proof ${radius_case} did not reach backend validation before GPU setup: "
            "${performance_proof_stdout}${performance_proof_stderr}")
    endif()
endforeach()

execute_process(
    COMMAND "${MC_MAIN}" --benchmark-game --workload ordinary-turning
        --gameplay-proof performance --render-distance 128
        --evidence "${OUTPUT_DIRECTORY}/invalid-performance-radius.json"
    RESULT_VARIABLE invalid_performance_radius_result
    OUTPUT_VARIABLE invalid_performance_radius_stdout
    ERROR_VARIABLE invalid_performance_radius_stderr
    TIMEOUT 10
)
if(invalid_performance_radius_result EQUAL 0
    OR NOT "${invalid_performance_radius_stdout}${invalid_performance_radius_stderr}"
        MATCHES "gameplay proof conflicts with render distance")
    message(FATAL_ERROR
        "obsolete 128-tile performance radius was not rejected by the parser: "
        "${invalid_performance_radius_stdout}${invalid_performance_radius_stderr}")
endif()

set(valid_scenario "${OUTPUT_DIRECTORY}/valid.core")
set(valid_evidence "${OUTPUT_DIRECTORY}/valid.json")
file(WRITE "${valid_scenario}" [=[@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(42u64)
    playerXYZ("alice", '@'c8, 4i32, 4i32, 10i32, 0i16, 0i16, 0i16)
    moveXYZ("alice", 1i8, 0i8, 0i8)
    wait(10u64)
    expectXYZ("alice", 9i32, 4i32, 10i32)
}
]=])
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${valid_scenario}" --evidence "${valid_evidence}"
    RESULT_VARIABLE valid_result
    OUTPUT_VARIABLE valid_stdout
    ERROR_VARIABLE valid_stderr
    TIMEOUT 10
)
if(NOT valid_result EQUAL 0)
    message(FATAL_ERROR
        "valid scenario command failed:\n"
        "command: \"${MC_MAIN}\" \"--scenario\" \"${valid_scenario}\" \"--evidence\" \"${valid_evidence}\"\n"
        "exit code: ${valid_result}\n"
        "stdout:\n${valid_stdout}\n"
        "stderr:\n${valid_stderr}"
    )
endif()
file(READ "${valid_evidence}" valid_json)
foreach(required_text
    "\"passed\": true"
    "\"ticks\": 10"
    "\"clients_accepted\": 1"
    "\"expectations_passed\": 1"
)
    string(FIND "${valid_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "valid scenario evidence is missing ${required_text}")
    endif()
endforeach()

execute_process(
    COMMAND "${MC_MAIN}" --scenario "${valid_scenario}" --evidence -
    RESULT_VARIABLE streamed_result
    OUTPUT_VARIABLE streamed_stdout
    ERROR_VARIABLE streamed_stderr
    TIMEOUT 10
)
if(NOT streamed_result EQUAL 0)
    message(FATAL_ERROR "stdout evidence command failed: ${streamed_stderr}")
endif()
string(FIND "${streamed_stdout}" "\nMC_RUNTIME_EVIDENCE_V1 " frame_start)
if(frame_start EQUAL -1)
    message(FATAL_ERROR "stdout evidence frame marker is missing")
endif()
math(EXPR frame_header_start "${frame_start} + 1")
string(SUBSTRING "${streamed_stdout}" ${frame_header_start} -1 frame_tail)
string(REGEX MATCH "^MC_RUNTIME_EVIDENCE_V1 ([0-9]+)\n" frame_header "${frame_tail}")
if(frame_header STREQUAL "")
    message(FATAL_ERROR "stdout evidence frame length is malformed")
endif()
set(frame_byte_count "${CMAKE_MATCH_1}")
string(LENGTH "${frame_header}" frame_header_length)
math(EXPR frame_payload_start "${frame_header_length}")
string(SUBSTRING "${frame_tail}" ${frame_payload_start} ${frame_byte_count} frame_json)
string(LENGTH "${frame_json}" frame_payload_length)
if(NOT frame_payload_length EQUAL frame_byte_count)
    message(FATAL_ERROR "stdout evidence frame is truncated")
endif()
math(EXPR frame_suffix_start "${frame_payload_start} + ${frame_byte_count}")
string(SUBSTRING "${frame_tail}" ${frame_suffix_start} -1 frame_suffix)
string(FIND "${frame_suffix}" "\nMC_RUNTIME_EVIDENCE_V1 " duplicate_frame_start)
if(NOT duplicate_frame_start EQUAL -1)
    message(FATAL_ERROR "stdout evidence contains more than one frame")
endif()
string(JSON streamed_schema ERROR_VARIABLE streamed_json_error GET "${frame_json}" schema_version)
if(NOT streamed_json_error STREQUAL "NOTFOUND"
    OR NOT streamed_schema STREQUAL "mc.runtime-evidence.v1")
    message(FATAL_ERROR "stdout evidence frame does not contain valid runtime JSON: ${streamed_json_error}")
endif()
foreach(required_text
    "\"passed\": true"
    "\"ticks\": 10"
    "\"clients_accepted\": 1"
    "\"expectations_passed\": 1"
)
    string(FIND "${frame_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "stdout evidence frame is missing ${required_text}")
    endif()
endforeach()

set(invalid_core_semicolon "${OUTPUT_DIRECTORY}/invalid-semicolon.core")
set(invalid_core_semicolon_evidence "${OUTPUT_DIRECTORY}/invalid-semicolon.json")
file(WRITE "${invalid_core_semicolon}" [=[@version(0.1.5);
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(42u64)
    playerXYZ("alice", '@'c8, 4i32, 4i32, 0i32, 0i16, 0i16, 0i16)
    moveXYZ("alice", 1i8, 0i8, 0i8)
    wait(10u64)
    expectXYZ("alice", 9i32, 4i32, 0i32)
}
]=])
execute_process(COMMAND "${MC_MAIN}" --scenario "${invalid_core_semicolon}" --evidence "${invalid_core_semicolon_evidence}" RESULT_VARIABLE invalid_core_semicolon_result TIMEOUT 10)
if(invalid_core_semicolon_result EQUAL 0)
    message(FATAL_ERROR "CoreLang 0.1.5 unexpectedly accepted a redundant header separator")
endif()

set(unknown_header "${OUTPUT_DIRECTORY}/unknown.core")
set(unknown_header_evidence "${OUTPUT_DIRECTORY}/unknown-core.json")
file(WRITE "${unknown_header}" "script 1;\n")
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${unknown_header}" --evidence "${unknown_header_evidence}"
    RESULT_VARIABLE unknown_header_result
    TIMEOUT 10
)
if(unknown_header_result EQUAL 0)
    message(FATAL_ERROR "unknown scenario header unexpectedly succeeded")
endif()
file(READ "${unknown_header_evidence}" unknown_header_json)
string(FIND "${unknown_header_json}" "unknown-source-header" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR "unknown scenario header diagnostic is missing")
endif()

set(mixed_header "${OUTPUT_DIRECTORY}/mixed.core")
set(mixed_header_evidence "${OUTPUT_DIRECTORY}/mixed-core.json")
file(WRITE "${mixed_header}" "scenario 1\n")
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${mixed_header}" --evidence "${mixed_header_evidence}"
    RESULT_VARIABLE mixed_header_result
    TIMEOUT 10
)
if(mixed_header_result EQUAL 0)
    message(FATAL_ERROR "legacy scenario header unexpectedly succeeded")
endif()
file(READ "${mixed_header_evidence}" mixed_header_json)
string(FIND "${mixed_header_json}" "unknown-source-header" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR "legacy scenario header diagnostic is missing")
endif()

set(invalid_scenario "${OUTPUT_DIRECTORY}/invalid.core")
set(invalid_evidence "${OUTPUT_DIRECTORY}/invalid.json")
file(WRITE "${invalid_scenario}" [=[@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(42u64)
    playerXYZ("alice", '@'c8, 4i32, 4i32, 0i32, 0i16, 0i16, 0i16)
    profile("flight3d-v1")
}
]=])
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${invalid_scenario}" --evidence "${invalid_evidence}"
    RESULT_VARIABLE invalid_result
    OUTPUT_VARIABLE invalid_stdout
    ERROR_VARIABLE invalid_stderr
    TIMEOUT 10
)
if(invalid_result EQUAL 0)
    message(FATAL_ERROR "invalid scenario command unexpectedly succeeded")
endif()
file(READ "${invalid_evidence}" invalid_json)
foreach(required_text "\"passed\": false" "corelang-runtime-failure")
    string(FIND "${invalid_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "invalid scenario evidence is missing ${required_text}")
    endif()
endforeach()

set(oversized_scenario "${OUTPUT_DIRECTORY}/oversized.core")
set(oversized_evidence "${OUTPUT_DIRECTORY}/oversized.json")
string(REPEAT "x" 65537 oversized_source)
file(WRITE "${oversized_scenario}" "${oversized_source}")
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${oversized_scenario}" --evidence "${oversized_evidence}"
    RESULT_VARIABLE oversized_result
    OUTPUT_VARIABLE oversized_stdout
    ERROR_VARIABLE oversized_stderr
    TIMEOUT 10
)
if(oversized_result EQUAL 0)
    message(FATAL_ERROR "oversized scenario command unexpectedly succeeded")
endif()
file(READ "${oversized_evidence}" oversized_json)
foreach(required_text "\"passed\": false" "65536-byte input limit before parsing")
    string(FIND "${oversized_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "oversized scenario evidence is missing ${required_text}")
    endif()
endforeach()

set(aliased_scenario "${OUTPUT_DIRECTORY}/aliased.core")
file(WRITE "${aliased_scenario}" [=[@version(0.1.5)
@use minecraft
pub fn scenario() {
    profile("flight3d-v1")
    seed(42u64)
    playerXYZ("alice", '@'c8, 4i32, 4i32, 0i32, 0i16, 0i16, 0i16)
}
]=])
file(READ "${aliased_scenario}" aliased_source_before)
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${aliased_scenario}"
        --evidence "${OUTPUT_DIRECTORY}/./aliased.core"
    RESULT_VARIABLE aliased_result
    OUTPUT_VARIABLE aliased_stdout
    ERROR_VARIABLE aliased_stderr
    TIMEOUT 5
)
if(aliased_result EQUAL 0)
    message(FATAL_ERROR "scenario/evidence path collision unexpectedly succeeded")
endif()
string(FIND "${aliased_stderr}" "scenario source and evidence paths must differ" aliased_error_index)
if(aliased_error_index EQUAL -1)
    message(FATAL_ERROR "scenario/evidence collision diagnostic is missing: ${aliased_stderr}")
endif()
file(READ "${aliased_scenario}" aliased_source_after)
if(NOT aliased_source_after STREQUAL aliased_source_before)
    message(FATAL_ERROR "scenario/evidence path collision modified the scenario source")
endif()

set(collision_output "${OUTPUT_DIRECTORY}/capture-evidence-collision.out")
file(REMOVE "${collision_output}")
execute_process(
    COMMAND "${MC_MAIN}" --capture-render --image "${collision_output}"
        --evidence "${OUTPUT_DIRECTORY}/./capture-evidence-collision.out"
    RESULT_VARIABLE collision_result
    OUTPUT_VARIABLE collision_stdout
    ERROR_VARIABLE collision_stderr
    TIMEOUT 5
)
if(collision_result EQUAL 0)
    message(FATAL_ERROR "capture/evidence path collision unexpectedly succeeded")
endif()
file(READ "${collision_output}" collision_json)
foreach(required_text "\"passed\": false" "capture image and evidence paths must differ")
    string(FIND "${collision_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "collision evidence is missing ${required_text}")
    endif()
endforeach()

foreach(capture_case IN ITEMS invalid-phase missing-image)
    if(capture_case STREQUAL "invalid-phase")
        set(capture_args --benchmark-frame-capture image.ppm --benchmark-frame-capture-phase invalid)
        set(expected_error "benchmark frame capture phase must be generation or playing")
    else()
        set(capture_args --benchmark-frame-capture-phase generation)
        set(expected_error "benchmark frame capture phase requires --benchmark-frame-capture")
    endif()
    execute_process(
        COMMAND "${MC_MAIN}" --benchmark-game ${capture_args} --evidence "${OUTPUT_DIRECTORY}/capture-invalid.json"
        RESULT_VARIABLE capture_result
        OUTPUT_VARIABLE capture_stdout
        ERROR_VARIABLE capture_stderr
        TIMEOUT 10
    )
    if(capture_result EQUAL 0 OR NOT "${capture_stdout}${capture_stderr}" MATCHES "${expected_error}")
        message(FATAL_ERROR "capture argument validation failed for ${capture_case}")
    endif()
endforeach()

file(WRITE "${OUTPUT_DIRECTORY}/capture-collision.ppm" "preserved-image")
execute_process(
    COMMAND "${MC_MAIN}" --benchmark-game --benchmark-frame-capture "${OUTPUT_DIRECTORY}/capture-collision.ppm"
        --evidence "${OUTPUT_DIRECTORY}/capture-collision.ppm"
    RESULT_VARIABLE collision_result
    OUTPUT_VARIABLE collision_stdout
    ERROR_VARIABLE collision_stderr
    TIMEOUT 10
)
file(READ "${OUTPUT_DIRECTORY}/capture-collision.ppm" collision_image)
if(collision_result EQUAL 0 OR NOT collision_image STREQUAL "preserved-image"
    OR NOT "${collision_stdout}${collision_stderr}" MATCHES "capture image and evidence paths must differ")
    message(FATAL_ERROR "capture collision did not preserve existing output")
endif()
execute_process(
    COMMAND "${MC_MAIN}" --benchmark-game --benchmark-frame-capture image.ppm
        --benchmark-frame-capture-delay-ms 30001 --evidence "${OUTPUT_DIRECTORY}/capture-invalid-delay.json"
    RESULT_VARIABLE delay_result
    OUTPUT_VARIABLE delay_stdout
    ERROR_VARIABLE delay_stderr
    TIMEOUT 10
)
if(delay_result EQUAL 0 OR NOT "${delay_stdout}${delay_stderr}" MATCHES "delay must be between zero and 30 seconds")
    message(FATAL_ERROR "capture delay range was not enforced")
endif()

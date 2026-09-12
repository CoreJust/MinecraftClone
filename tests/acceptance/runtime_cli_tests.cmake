if(NOT DEFINED MC_MAIN OR NOT DEFINED OUTPUT_DIRECTORY)
    message(FATAL_ERROR "MC_MAIN and OUTPUT_DIRECTORY are required")
endif()

file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")

set(valid_scenario "${OUTPUT_DIRECTORY}/valid.mcscenario")
set(valid_evidence "${OUTPUT_DIRECTORY}/valid.json")
file(WRITE "${valid_scenario}" [=[scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
begin
input alice 1 0
wait 10
expect player alice position 5 4
end
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

set(valid_core_semicolon "${OUTPUT_DIRECTORY}/valid-semicolon.core")
set(valid_core_semicolon_evidence "${OUTPUT_DIRECTORY}/valid-semicolon.json")
file(WRITE "${valid_core_semicolon}" [=[@version("0.0.1");
@use MinecraftScenario
profile("flat2d-v1")
seed(42u64)
player("alice", '@', 4u8, 4u8)
input("alice", 1i8, 0i8)
wait(10u64)
expect_position("alice", 5u8, 4u8)
]=])
execute_process(COMMAND "${MC_MAIN}" --scenario "${valid_core_semicolon}" --evidence "${valid_core_semicolon_evidence}" RESULT_VARIABLE valid_core_semicolon_result TIMEOUT 10)
if(NOT valid_core_semicolon_result EQUAL 0)
    message(FATAL_ERROR "semicolon-terminated CoreLang header failed")
endif()

set(valid_core "${OUTPUT_DIRECTORY}/valid.core")
set(valid_core_evidence "${OUTPUT_DIRECTORY}/valid-core.json")
file(WRITE "${valid_core}" [=[@version("0.0.1")
@use MinecraftScenario
profile("flat2d-v1")
seed(42u64)
player("alice", '@', 4u8, 4u8)
input("alice", 1i8, 0i8)
wait(10u64)
expect_position("alice", 5u8, 4u8)
]=])
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${valid_core}" --evidence "${valid_core_evidence}"
    RESULT_VARIABLE valid_core_result
    ERROR_VARIABLE valid_core_stderr
    TIMEOUT 10
)
if(NOT valid_core_result EQUAL 0)
    message(FATAL_ERROR "valid CoreLang scenario command failed: ${valid_core_stderr}")
endif()
file(READ "${valid_core_evidence}" valid_core_json)
foreach(required_text "\"passed\": true" "\"ticks\": 10" "\"clients_accepted\": 1")
    string(FIND "${valid_core_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "valid CoreLang evidence is missing ${required_text}")
    endif()
endforeach()

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
file(WRITE "${mixed_header}" "@version(\"0.0.1\")\nscenario 1\n")
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${mixed_header}" --evidence "${mixed_header_evidence}"
    RESULT_VARIABLE mixed_header_result
    TIMEOUT 10
)
if(mixed_header_result EQUAL 0)
    message(FATAL_ERROR "mixed scenario headers unexpectedly succeeded")
endif()
file(READ "${mixed_header_evidence}" mixed_header_json)
string(FIND "${mixed_header_json}" "corelang-compile-failure" match_index)
if(match_index EQUAL -1)
    message(FATAL_ERROR "mixed scenario header diagnostic is missing")
endif()

set(invalid_scenario "${OUTPUT_DIRECTORY}/invalid.mcscenario")
set(invalid_evidence "${OUTPUT_DIRECTORY}/invalid.json")
file(WRITE "${invalid_scenario}" [=[scenario 1
profile "flat2d-v1"
seed 42
player alice character "@" at 4 4
begin
end
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
foreach(required_text "\"passed\": false" "malformed-syntax")
    string(FIND "${invalid_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "invalid scenario evidence is missing ${required_text}")
    endif()
endforeach()

set(oversized_scenario "${OUTPUT_DIRECTORY}/oversized.mcscenario")
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

set(aliased_scenario "${OUTPUT_DIRECTORY}/aliased.mcscenario")
file(WRITE "${aliased_scenario}" [=[scenario 1
profile flat2d-v1
seed 42
player alice character "@" at 4 4
begin
end
]=])
file(READ "${aliased_scenario}" aliased_source_before)
execute_process(
    COMMAND "${MC_MAIN}" --scenario "${aliased_scenario}"
        --evidence "${OUTPUT_DIRECTORY}/./aliased.mcscenario"
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

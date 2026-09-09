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
wait 5
expect player alice position 9 4
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
    message(FATAL_ERROR "valid scenario command failed: ${valid_stderr}")
endif()
file(READ "${valid_evidence}" valid_json)
foreach(required_text
    "\"passed\": true"
    "\"ticks\": 5"
    "\"clients_accepted\": 1"
    "\"expectations_passed\": 1"
)
    string(FIND "${valid_json}" "${required_text}" match_index)
    if(match_index EQUAL -1)
        message(FATAL_ERROR "valid scenario evidence is missing ${required_text}")
    endif()
endforeach()

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

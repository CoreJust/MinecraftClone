# Keep timed acceptance cadence tests independent from unrelated CTest work.
foreach(serial_runtime_test IN ITEMS
    GameServerPreviewTest.ResetsPriorityCursorAfterProgressWhenHeadingChanges
    GameClientLifecycleTest.StopDuringRetryIsServicedWithoutStartingAnotherAttempt
    GameServerPreviewTest.SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream
)
    list(FIND mc_tests_TESTS "${serial_runtime_test}" serial_runtime_test_index)
    if(NOT serial_runtime_test_index EQUAL -1)
        set_tests_properties("${serial_runtime_test}" PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

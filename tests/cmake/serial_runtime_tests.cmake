# Keep timed acceptance cadence tests independent from unrelated CTest work.
foreach(serial_runtime_test IN ITEMS
    GameServerPreviewTest.ResetsPriorityCursorAfterProgressWhenHeadingChanges
    GameClientLifecycleTest.StopDuringRetryIsServicedWithoutStartingAnotherAttempt
    GameServerPreviewTest.SustainedFlightKeepsInputAcknowledgementsCurrentWhileTilesStream
)
    if(serial_runtime_test IN_LIST mc_tests_TESTS)
        set_tests_properties("${serial_runtime_test}" PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

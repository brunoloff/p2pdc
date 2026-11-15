cmake_minimum_required(VERSION 3.20)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/cmake_opendht_embedded"
            -B "${TEST_BINARY_DIR}/test-datachannel-missing" -DCASE=missing_rtc
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "LibDataChannel" OR NOT errors MATCHES "REQUIRED")
    message(FATAL_ERROR "missing libdatachannel did not fail as expected: ${output}\n${errors}")
endif()

cmake_minimum_required(VERSION 3.20)
if(NOT CASE STREQUAL "provided" AND NOT CASE STREQUAL "missing")
    message(FATAL_ERROR "unknown OpenDHT target test case")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/cmake_opendht_target"
            -B "${TEST_BINARY_DIR}/test-opendht-${CASE}" "-DCASE=${CASE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(CASE STREQUAL "provided" AND NOT result EQUAL 0)
    message(FATAL_ERROR "provided OpenDHT target failed: ${output}\n${errors}")
endif()
if(CASE STREQUAL "missing" AND
   (result EQUAL 0 OR NOT errors MATCHES "P2PDC_OPENDHT_TARGET does not name an existing target"))
    message(FATAL_ERROR "missing OpenDHT target did not fail as expected: ${output}\n${errors}")
endif()

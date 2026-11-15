add_test(NAME p2pdc_native_defaults
         COMMAND ${CMAKE_COMMAND} -DCASE=native -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_native_defaults_test.cmake)
add_test(NAME p2pdc_browser_defaults
         COMMAND ${CMAKE_COMMAND} -DCASE=browser -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_native_defaults_test.cmake)
add_test(
    NAME p2pdc_native_overrides
    COMMAND
        ${CMAKE_COMMAND} -DCASE=overrides -DP2PDC_ENABLE_OPENDHT=OFF
        -DP2PDC_ENABLE_NATIVE_TRANSPORT=OFF -P
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_native_defaults_test.cmake)
add_test(NAME p2pdc_browser_test_override
         COMMAND ${CMAKE_COMMAND} -DCASE=browser_override -DBUILD_TESTING=ON -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_native_defaults_test.cmake)
add_test(NAME p2pdc_opendht_target
         COMMAND ${CMAKE_COMMAND} -DCASE=provided -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR} -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_opendht_target_test.cmake)
add_test(NAME p2pdc_opendht_missing_target
         COMMAND ${CMAKE_COMMAND} -DCASE=missing -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR} -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_opendht_target_test.cmake)
add_test(NAME p2pdc_opendht_embedded_target
         COMMAND ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_opendht_embedded
                 -B ${CMAKE_CURRENT_BINARY_DIR}/test-opendht-embedded)
add_test(NAME p2pdc_datachannel_missing_package
         COMMAND ${CMAKE_COMMAND} -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR} -P
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake_datachannel_missing_test.cmake)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    foreach(vector_test IN ITEMS frame_vectors discovery_vectors)
        add_test(NAME p2pdc_${vector_test}_python
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/${vector_test}_test.py)
    endforeach()
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import nacl"
                    RESULT_VARIABLE p2pdc_pynacl_status OUTPUT_QUIET ERROR_QUIET)
    if(p2pdc_pynacl_status EQUAL 0)
        foreach(vector_test IN ITEMS advertisement_vector signaling_vector handshake_vector)
            add_test(NAME p2pdc_${vector_test}_python
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/${vector_test}_test.py)
        endforeach()
    else()
        message(STATUS "PyNaCl unavailable: cryptographic Python vector checks are omitted")
    endif()
else()
    message(STATUS "Python unavailable: independent Python vector checks are omitted")
endif()

# Tests of the native network discovery backends (cmake/NetworkBackends.cmake). The store and
# node tests run against the JavaScript test servers in tests/support/, started with node.
if(NOT P2PDC_ENABLE_NETWORK_BACKENDS)
    return()
endif()
find_program(P2PDC_TEST_NODE_EXECUTABLE node)
add_executable(p2pdc_backend_tests tests/backend_unit_test.cpp tests/fan_out_store_test.cpp)
if(P2PDC_TEST_NODE_EXECUTABLE AND UNIX)
    target_sources(p2pdc_backend_tests PRIVATE tests/network_store_test.cpp tests/peerjs_store_test.cpp)
    target_compile_definitions(p2pdc_backend_tests PRIVATE
        "P2PDC_TEST_NODE=\"${P2PDC_TEST_NODE_EXECUTABLE}\"")
else()
    message(STATUS "node not found: skipping the native tests against local discovery servers")
endif()
target_compile_definitions(p2pdc_backend_tests PRIVATE
    "P2PDC_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
target_include_directories(p2pdc_backend_tests PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(p2pdc_backend_tests PRIVATE p2pdc::p2pdc nlohmann_json::nlohmann_json
    LibDataChannel::LibDataChannel GTest::gtest_main)
target_compile_options(p2pdc_backend_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
gtest_discover_tests(p2pdc_backend_tests PROPERTIES TIMEOUT 120)

# Whole nodes that meet only through a backend: each runs a DHT of its own.
if(P2PDC_TEST_NODE_EXECUTABLE AND UNIX AND P2PDC_TEST_NATIVE_TRANSPORT AND P2PDC_TEST_OPENDHT)
    add_executable(p2pdc_network_node_tests tests/native_network_node_test.cpp
        tests/echo_example_pair.cpp examples/echo/echo_peer.cpp)
    target_compile_definitions(p2pdc_network_node_tests PRIVATE
        "P2PDC_TEST_NODE=\"${P2PDC_TEST_NODE_EXECUTABLE}\""
        "P2PDC_TEST_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")
    target_link_libraries(p2pdc_network_node_tests PRIVATE p2pdc::p2pdc
        nlohmann_json::nlohmann_json GTest::gtest_main)
    target_compile_options(p2pdc_network_node_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
    gtest_discover_tests(p2pdc_network_node_tests PROPERTIES TIMEOUT 120)
endif()

# Against public servers; opt-in through P2PDC_TEST_NOSTR_RELAYS and P2PDC_TEST_MQTT_BROKERS.
add_executable(p2pdc_backend_live_tests EXCLUDE_FROM_ALL tests/network_store_live_test.cpp)
target_include_directories(p2pdc_backend_live_tests PRIVATE ${SODIUM_INCLUDE_DIR})
target_link_libraries(p2pdc_backend_live_tests PRIVATE p2pdc::p2pdc nlohmann_json::nlohmann_json
    LibDataChannel::LibDataChannel GTest::gtest_main)
target_compile_options(p2pdc_backend_live_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)

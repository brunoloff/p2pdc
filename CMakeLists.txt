cmake_minimum_required(VERSION 3.20)
project(p2p_discovery_connect VERSION 0.3.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)

option(P2PDC_BUILD_DEPENDENCIES "Download and build only the dependencies; see deps/" OFF)
if(P2PDC_BUILD_DEPENDENCIES)
    add_subdirectory(deps)
    return()
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# One shared Ninja pool for compiling and linking keeps builds from saturating the host, unless
# CMAKE_BUILD_PARALLEL_LEVEL asks for more.
set(p2pdc_heavy_jobs 1)
if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    set(p2pdc_heavy_jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
endif()
set_property(GLOBAL APPEND PROPERTY JOB_POOLS p2pdc_heavy=${p2pdc_heavy_jobs})
set(CMAKE_JOB_POOL_COMPILE p2pdc_heavy)
set(CMAKE_JOB_POOL_LINK p2pdc_heavy)
include(cmake/CompilerFlags.cmake)
include(cmake/NativeOptions.cmake)
include(CTest)
option(P2PDC_TEST_OPENDHT "Run local OpenDHT integration tests" OFF)
option(P2PDC_TEST_NATIVE_TRANSPORT "Run local libdatachannel integration tests" OFF)
option(P2PDC_ENABLE_SODIUM "Build libsodium security primitives" ON)
option(P2PDC_BUILD_BROWSER_RELIABLE "Build experimental WebAssembly reliable bridge" OFF)
option(P2PDC_BUILD_BROWSER_SECURITY_TEST "Build browser handshake smoke test" OFF)
option(P2PDC_BUILD_BROWSER_SECURE "Build experimental authenticated WebAssembly bridge" OFF)
# The example needs Qt, Yrs and every backend, so it is built by default only
# when p2pdc is the top-level project, not when a consumer embeds it.
option(P2PDC_BUILD_SHARED_TEXT "Build the Qt Quick shared text example" ${p2pdc_is_top_level})
if(P2PDC_TEST_NATIVE_TRANSPORT AND NOT P2PDC_ENABLE_NATIVE_TRANSPORT)
    message(FATAL_ERROR "P2PDC_TEST_NATIVE_TRANSPORT requires P2PDC_ENABLE_NATIVE_TRANSPORT")
endif()

if(P2PDC_ENABLE_NATIVE_TRANSPORT AND NOT TARGET LibDataChannel::LibDataChannel)
    find_package(LibDataChannel CONFIG REQUIRED)
endif()

if(P2PDC_ENABLE_OPENDHT OR (BUILD_TESTING AND P2PDC_TEST_OPENDHT))
    include(cmake/OpenDhtDependency.cmake)
endif()

add_library(p2pdc
    src/browser/browser_ice_config.cpp
    src/core/callback_gate.cpp
    src/core/backend_drain.cpp
    src/core/deferred_events.cpp
    src/core/shutdown_coordinator.cpp
    src/core/timer_producer.cpp
    src/core/memory_budget.cpp src/core/ice_config_validation.cpp
    src/core/route.cpp
    src/core/round_trip.cpp
    src/core/wall_clock.cpp
    src/discovery/incoming.cpp
    src/discovery/native_mailbox.cpp src/discovery/recovering_mailbox.cpp
    src/discovery/native_outbox.cpp
    src/discovery/native_publisher.cpp
    src/discovery/native_signal_pipe.cpp
    src/discovery/record_store.cpp
    src/discovery/discovery_backends.cpp
    src/discovery/subscription.cpp
    src/discovery/signaling_queue.cpp
    src/protocol/frame.cpp
    src/protocol/fragmenter.cpp
    src/protocol/reassembler.cpp
    src/protocol/reliable_channel.cpp
    src/protocol/reliable_pump.cpp
    src/protocol/reliable_receiver.cpp
    src/protocol/reliable_sender.cpp
    src/protocol/send_queue.cpp
    src/transport/inbound_queue.cpp
    src/transport/connection.cpp
)
add_library(p2pdc::p2pdc ALIAS p2pdc)
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_sources(p2pdc PRIVATE src/core/runtime.cpp)
    target_link_libraries(p2pdc PUBLIC Threads::Threads)
else()
    target_sources(p2pdc PRIVATE src/browser/wasm_runtime.cpp src/browser/browser_record_store.cpp src/browser/browser_transport.cpp)
    target_compile_options(p2pdc PUBLIC -fexceptions)
    set(p2pdc_proxy_sources
        src/browser/proxy_codec.mjs src/browser/proxy_stream.mjs src/browser/store_limits.mjs
        src/browser/opendht_proxy.mjs src/browser/failover_proxy.mjs
        src/browser/websocket_channel.mjs src/browser/schnorr.mjs src/browser/nostr_relay.mjs
        src/browser/mqtt_codec.mjs src/browser/mqtt_broker.mjs
        src/browser/peerjs_socket.mjs src/browser/peerjs_link.mjs src/browser/peerjs_endpoint.mjs
        src/browser/peerjs_host.mjs src/browser/peerjs_switchboard.mjs
        src/browser/fan_out_store.mjs src/browser/discovery_backends.mjs
        src/browser/polling_proxy_watch.mjs src/browser/fallback_proxy_watch.mjs
        src/browser/shared_proxy_watch.mjs src/browser/record_store_bridge.mjs
        src/browser/record_store_prelude.js)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${p2pdc_proxy_sources})
    set(p2pdc_proxy_bundle "Module.p2pdcRecordBridge = (() => {\n")
    foreach(p2pdc_source IN LISTS p2pdc_proxy_sources)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${p2pdc_source}" p2pdc_proxy_part)
        string(REGEX REPLACE "(^|\n)import [^\n]*\n" "\n" p2pdc_proxy_part "${p2pdc_proxy_part}")
        string(REGEX REPLACE "export \\{[^\n]*\n" "" p2pdc_proxy_part "${p2pdc_proxy_part}")
        string(REGEX REPLACE "(^|\n)export (async function|function|class|const) " "\\1\\2 "
            p2pdc_proxy_part "${p2pdc_proxy_part}")
        if(p2pdc_proxy_part MATCHES "(^|\n)[ \t]*(import|export)[ \t]")
            message(FATAL_ERROR "Unsupported module syntax in ${p2pdc_source}; update the browser bundle")
        endif()
        string(APPEND p2pdc_proxy_bundle "${p2pdc_proxy_part}\n")
    endforeach()
    string(APPEND p2pdc_proxy_bundle "})();\n")
    set(p2pdc_proxy_prelude "${CMAKE_CURRENT_BINARY_DIR}/p2pdc_proxy_prelude.js")
    if(EXISTS "${p2pdc_proxy_prelude}")
        file(READ "${p2pdc_proxy_prelude}" p2pdc_proxy_previous)
    endif()
    if(NOT "${p2pdc_proxy_previous}" STREQUAL "${p2pdc_proxy_bundle}")
        file(WRITE "${p2pdc_proxy_prelude}" "${p2pdc_proxy_bundle}")
    endif()
    find_program(P2PDC_NODE_EXECUTABLE node REQUIRED)
    execute_process(COMMAND "${P2PDC_NODE_EXECUTABLE}" --check
        "${p2pdc_proxy_prelude}"
        RESULT_VARIABLE p2pdc_proxy_status ERROR_VARIABLE p2pdc_proxy_error)
    if(NOT p2pdc_proxy_status EQUAL 0)
        message(FATAL_ERROR "Invalid browser proxy bundle: ${p2pdc_proxy_error}")
    endif()
    set_property(TARGET p2pdc APPEND PROPERTY INTERFACE_LINK_DEPENDS "${p2pdc_proxy_prelude}")
    set(p2pdc_rtc_sources src/browser/webrtc_datagrams.mjs src/browser/webrtc_candidates.mjs src/browser/webrtc_relay_filter.mjs src/browser/webrtc_route.mjs src/browser/webrtc_settings.mjs src/browser/webrtc_stats.mjs
        src/browser/webrtc_transport.mjs src/browser/webrtc_bridge.mjs src/browser/webrtc_prelude.js)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${p2pdc_rtc_sources})
    set(p2pdc_rtc_bundle "Module.p2pdcWebRtcBridge = (() => {\n")
    foreach(p2pdc_source IN LISTS p2pdc_rtc_sources)
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/${p2pdc_source}" p2pdc_rtc_part)
        string(REGEX REPLACE "(^|\n)import [^\n]*\n" "\n" p2pdc_rtc_part "${p2pdc_rtc_part}")
        string(REGEX REPLACE "(^|\n)export (async function|function|class) " "\\1\\2 "
            p2pdc_rtc_part "${p2pdc_rtc_part}")
        if(p2pdc_rtc_part MATCHES "(^|\n)[ \t]*(import|export)[ \t]")
            message(FATAL_ERROR "Unsupported module syntax in ${p2pdc_source}; update the browser bundle")
        endif()
        string(APPEND p2pdc_rtc_bundle "${p2pdc_rtc_part}\n")
    endforeach()
    string(APPEND p2pdc_rtc_bundle "})();\n")
    set(p2pdc_rtc_prelude "${CMAKE_CURRENT_BINARY_DIR}/p2pdc_webrtc_prelude.js")
    if(EXISTS "${p2pdc_rtc_prelude}")
        file(READ "${p2pdc_rtc_prelude}" p2pdc_rtc_previous)
    endif()
    if(NOT "${p2pdc_rtc_previous}" STREQUAL "${p2pdc_rtc_bundle}")
        file(WRITE "${p2pdc_rtc_prelude}" "${p2pdc_rtc_bundle}")
    endif()
    execute_process(COMMAND "${P2PDC_NODE_EXECUTABLE}" --check "${p2pdc_rtc_prelude}"
        RESULT_VARIABLE p2pdc_rtc_status ERROR_VARIABLE p2pdc_rtc_error)
    if(NOT p2pdc_rtc_status EQUAL 0)
        message(FATAL_ERROR "Invalid browser WebRTC bundle: ${p2pdc_rtc_error}")
    endif()
    set_property(TARGET p2pdc APPEND PROPERTY INTERFACE_LINK_DEPENDS "${p2pdc_rtc_prelude}")
    target_link_options(p2pdc INTERFACE -fexceptions "-sDISABLE_EXCEPTION_CATCHING=0"
        "SHELL:--pre-js \"${p2pdc_proxy_prelude}\""
        "SHELL:--pre-js \"${p2pdc_rtc_prelude}\"")
endif()
target_compile_features(p2pdc PUBLIC cxx_std_20)
target_include_directories(p2pdc PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_options(p2pdc PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>
)
p2pdc_apply_hardening(p2pdc)

if(P2PDC_BUILD_BROWSER_RELIABLE)
    if(NOT EMSCRIPTEN)
        message(FATAL_ERROR "P2PDC_BUILD_BROWSER_RELIABLE requires Emscripten")
    endif()
    add_executable(p2pdc_browser_reliable src/browser/wasm_reliable.cpp)
    target_link_libraries(p2pdc_browser_reliable PRIVATE p2pdc::p2pdc)
    target_compile_options(p2pdc PRIVATE -fexceptions)
    target_compile_options(p2pdc_browser_reliable PRIVATE -fexceptions)
    target_link_options(p2pdc_browser_reliable PRIVATE --no-entry -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0"
        "-sMODULARIZE=1" "-sEXPORT_ES6=1" "-sENVIRONMENT=web"
        "-sALLOW_MEMORY_GROWTH=1" "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']")
    set_target_properties(p2pdc_browser_reliable PROPERTIES SUFFIX ".mjs")
endif()

if(P2PDC_ENABLE_SODIUM)
    find_path(SODIUM_INCLUDE_DIR sodium.h REQUIRED)
    find_library(SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
    target_sources(p2pdc PRIVATE src/core/attempt_guard.cpp src/security/security.cpp src/security/recipient_crypto.cpp src/security/handshake.cpp src/security/handshake_exchange.cpp src/security/handshake_gate.cpp src/security/async_signaling.cpp src/security/secure_channel.cpp src/security/secure_attempt.cpp
        src/security/signaling.cpp src/security/signaling_session.cpp src/security/signaling_candidate.cpp src/security/signaling_flow.cpp
        src/discovery/advertisement.cpp src/discovery/advertisement_tracker.cpp
        src/discovery/incoming_offer_resolver.cpp src/discovery/peer_admission.cpp
        src/discovery/discovery_key.cpp src/discovery/discovery_window.cpp
        src/discovery/native_discovery_watch.cpp src/discovery/native_windowed_mailbox.cpp
        src/discovery/native_advertisement_lease.cpp src/discovery/native_discovery_cycle.cpp
        src/discovery/endpoint_identity.cpp src/discovery/native_public_watch.cpp src/discovery/native_public_lease.cpp
        src/discovery/native_incoming_listener.cpp src/discovery/native_public_incoming.cpp src/discovery/public_incoming_overflow.cpp src/discovery/terminal_signal.cpp src/discovery/collision_registry.cpp)
    target_include_directories(p2pdc PRIVATE ${SODIUM_INCLUDE_DIR})
    target_link_libraries(p2pdc PUBLIC ${SODIUM_LIBRARY})
endif()

if(EMSCRIPTEN AND P2PDC_ENABLE_SODIUM)
    target_sources(p2pdc PRIVATE src/discovery/native_node.cpp src/discovery/native_node_identity.cpp
        src/discovery/native_node_connect.cpp src/discovery/native_node_incoming.cpp src/discovery/connection_admission.cpp
        src/browser/browser_node_platform.cpp src/browser/browser_secure_attempt.cpp src/browser/browser_secure_callbacks.cpp src/browser/browser_mailbox_attempt.cpp src/browser/browser_mailbox_callbacks.cpp src/browser/browser_public_attempt.cpp src/browser/browser_public_config.cpp src/transport/attempt_binding.cpp src/transport/native_public_connection.cpp src/transport/datagram_delivery.cpp src/transport/native_connection_validation.cpp)
    target_compile_definitions(p2pdc PRIVATE P2PDC_HAVE_DISCOVERY)
    set_property(SOURCE src/discovery/native_node_incoming.cpp APPEND PROPERTY COMPILE_DEFINITIONS P2PDC_HAVE_INCOMING_LISTENER P2PDC_HAVE_PUBLIC_CONNECT)
    set_property(SOURCE src/discovery/native_node_connect.cpp APPEND PROPERTY COMPILE_DEFINITIONS P2PDC_HAVE_PUBLIC_CONNECT)
endif()

if(P2PDC_BUILD_BROWSER_SECURITY_TEST)
    if(NOT EMSCRIPTEN OR NOT P2PDC_ENABLE_SODIUM)
        message(FATAL_ERROR "Browser security test requires Emscripten and libsodium")
    endif()
    add_executable(p2pdc_browser_handshake_smoke tests/browser_handshake_smoke.cpp tests/browser_secure_channel_smoke.cpp tests/browser_public_config_smoke.cpp tests/portable_admission_smoke.cpp tests/record_store_smoke.cpp)
    target_link_libraries(p2pdc_browser_handshake_smoke PRIVATE p2pdc::p2pdc)
    target_compile_options(p2pdc PRIVATE -fexceptions)
    target_compile_options(p2pdc_browser_handshake_smoke PRIVATE -fexceptions)
    target_link_options(p2pdc_browser_handshake_smoke PRIVATE -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sMODULARIZE=1" "-sEXPORT_ES6=1"
        "-sENVIRONMENT=web" "-sALLOW_MEMORY_GROWTH=1")
    set_target_properties(p2pdc_browser_handshake_smoke PROPERTIES SUFFIX ".mjs")
    add_executable(p2pdc_browser_runtime_smoke tests/browser_runtime_smoke.cpp)
    target_link_libraries(p2pdc_browser_runtime_smoke PRIVATE p2pdc::p2pdc)
    target_compile_options(p2pdc_browser_runtime_smoke PRIVATE ${p2pdc_warning_flags})
    target_link_options(p2pdc_browser_runtime_smoke PRIVATE "-sNO_EXIT_RUNTIME=1")
    add_executable(p2pdc_browser_record_store_smoke tests/browser_record_store_smoke.cpp tests/browser_terminal_retention_smoke.cpp tests/browser_public_discovery_probe.cpp tests/browser_mailbox_attempt_smoke.cpp tests/browser_mailbox_failure_smoke.cpp tests/portable_discovery_smoke.cpp tests/record_store_smoke.cpp)
    target_link_libraries(p2pdc_browser_record_store_smoke PRIVATE p2pdc::p2pdc)
    target_compile_options(p2pdc_browser_record_store_smoke PRIVATE ${p2pdc_warning_flags})
    target_link_options(p2pdc_browser_record_store_smoke PRIVATE "-sNO_EXIT_RUNTIME=1")
    add_executable(p2pdc_browser_transport_smoke tests/browser_transport_main.cpp tests/browser_transport_smoke.cpp tests/browser_secure_attempt_smoke.cpp)
    target_link_libraries(p2pdc_browser_transport_smoke PRIVATE p2pdc::p2pdc)
    target_compile_options(p2pdc_browser_transport_smoke PRIVATE ${p2pdc_warning_flags})
    target_link_options(p2pdc_browser_transport_smoke PRIVATE "-sNO_EXIT_RUNTIME=1")
    add_executable(p2pdc_browser_public_connection_smoke tests/browser_public_connection_main.cpp tests/node_connection_consumer.cpp tests/node_ice_validation_smoke.cpp tests/node_race_consumer.cpp tests/node_mixed_race.cpp tests/browser_native_public_peer.cpp tests/echo_example_pair.cpp examples/echo/echo_peer.cpp)
    target_link_libraries(p2pdc_browser_public_connection_smoke PRIVATE p2pdc::p2pdc)
    target_compile_options(p2pdc_browser_public_connection_smoke PRIVATE ${p2pdc_warning_flags})
    target_link_options(p2pdc_browser_public_connection_smoke PRIVATE "-sNO_EXIT_RUNTIME=1")
endif()

if(P2PDC_BUILD_BROWSER_SECURE)
    if(NOT EMSCRIPTEN OR NOT P2PDC_ENABLE_SODIUM)
        message(FATAL_ERROR "Browser secure bridge requires Emscripten and libsodium")
    endif()
    add_executable(p2pdc_browser_secure src/browser/wasm_secure_attempt.cpp src/browser/wasm_secure_exports.cpp
        src/browser/wasm_discovery.cpp src/browser/wasm_discovery_exports.cpp)
    target_link_libraries(p2pdc_browser_secure PRIVATE p2pdc::p2pdc)
    target_include_directories(p2pdc_browser_secure PRIVATE ${SODIUM_INCLUDE_DIR})
    target_compile_options(p2pdc PRIVATE -fexceptions)
    target_compile_options(p2pdc_browser_secure PRIVATE -fexceptions ${p2pdc_warning_flags})
    target_link_options(p2pdc_browser_secure PRIVATE --no-entry -fexceptions
        "-sDISABLE_EXCEPTION_CATCHING=0" "-sMODULARIZE=1" "-sEXPORT_ES6=1"
        "-sENVIRONMENT=web" "-sALLOW_MEMORY_GROWTH=1" "-sEXPORTED_FUNCTIONS=['_malloc','_free']"
        "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']")
    set_target_properties(p2pdc_browser_secure PROPERTIES SUFFIX ".mjs")
endif()

if(P2PDC_ENABLE_OPENDHT)
    target_sources(p2pdc PRIVATE src/discovery/native_node.cpp src/discovery/native_node_platform.cpp src/discovery/native_node_identity.cpp src/discovery/native_node_connect.cpp src/discovery/native_node_incoming.cpp src/discovery/native_runner_owner.cpp src/discovery/native_runner_start.cpp src/discovery/native_store.cpp src/discovery/native_record_store.cpp)
    if(P2PDC_ENABLE_SODIUM)
        target_sources(p2pdc PRIVATE src/discovery/connection_admission.cpp)
        target_compile_definitions(p2pdc PRIVATE P2PDC_HAVE_DISCOVERY)
    endif()
    target_link_libraries(p2pdc PRIVATE ${p2pdc_opendht_target} msgpack-cxx)
endif()

if(P2PDC_ENABLE_NATIVE_TRANSPORT)
    target_sources(p2pdc PRIVATE src/transport/native_transport.cpp src/transport/native_datagram_channel.cpp src/transport/relay_candidate_filter.cpp src/transport/native_reliable_link.cpp src/transport/native_ice_config.cpp)
    if(P2PDC_ENABLE_SODIUM)
        target_sources(p2pdc PRIVATE src/transport/native_signaling.cpp)
        if(P2PDC_ENABLE_OPENDHT)
            target_sources(p2pdc PRIVATE src/transport/native_attempt.cpp src/transport/native_attempt_pipe.cpp src/transport/native_outgoing_config.cpp src/transport/native_incoming_config.cpp src/transport/native_public_connection.cpp src/transport/datagram_delivery.cpp src/transport/native_public_attempt.cpp src/transport/native_connection_validation.cpp src/transport/attempt_binding.cpp)
            set_source_files_properties(src/discovery/native_node_connect.cpp PROPERTIES COMPILE_DEFINITIONS P2PDC_HAVE_PUBLIC_CONNECT)
            set_property(SOURCE src/discovery/native_node_incoming.cpp APPEND PROPERTY COMPILE_DEFINITIONS P2PDC_HAVE_PUBLIC_CONNECT P2PDC_HAVE_INCOMING_LISTENER)
        endif()
    endif()
    target_link_libraries(p2pdc PRIVATE LibDataChannel::LibDataChannel)
endif()

include(cmake/NetworkBackends.cmake)
include(cmake/EchoStandalone.cmake)
if(BUILD_TESTING)
    find_package(GTest REQUIRED)
    include(GoogleTest)
    include(cmake/NativeOptionsTests.cmake)
    include(cmake/NetworkBackendsTests.cmake)
    if(P2PDC_ENABLE_OPENDHT AND UNIX AND NOT APPLE)
        add_executable(p2pdc_browser_record_native_peer EXCLUDE_FROM_ALL tests/browser_record_native_peer.cpp)
        target_link_libraries(p2pdc_browser_record_native_peer PRIVATE p2pdc::p2pdc ${p2pdc_opendht_target} msgpack-cxx)
        target_compile_options(p2pdc_browser_record_native_peer PRIVATE ${p2pdc_warning_flags})
    endif()
    add_executable(p2pdc_tests
        tests/discovery_backends_test.cpp
        tests/callback_gate_test.cpp
        tests/backend_drain_test.cpp
        tests/deferred_events_test.cpp
        tests/shutdown_coordinator_test.cpp
        tests/timer_producer_test.cpp
        tests/submission_gate_test.cpp
        tests/inbound_queue_test.cpp
        tests/memory_budget_test.cpp
        tests/wall_clock_test.cpp
        tests/callback_failure_test.cpp
        tests/rate_window_test.cpp
        tests/join_retry_test.cpp
        tests/operation_test.cpp
        tests/route_test.cpp tests/round_trip_test.cpp
        tests/browser_ice_config_test.cpp
        tests/record_store_smoke.cpp
        tests/record_store_test.cpp
        tests/frame_test.cpp
        tests/fragmenter_test.cpp
        tests/reassembler_test.cpp tests/recovering_mailbox_test.cpp tests/announcement_budget_test.cpp
        tests/reliable_channel_test.cpp
        tests/reliable_pump_test.cpp
        tests/reliable_receiver_test.cpp
        tests/reliable_sender_test.cpp
        tests/send_queue_test.cpp
        tests/signaling_queue_test.cpp
    )
    if(NOT EMSCRIPTEN)
        target_sources(p2pdc_tests PRIVATE tests/runtime_test.cpp)
    endif()
    if(P2PDC_ENABLE_SODIUM)
        target_sources(p2pdc_tests PRIVATE tests/discovery_recovery_test.cpp tests/portable_discovery_smoke.cpp tests/portable_discovery_test.cpp tests/portable_admission_smoke.cpp tests/portable_admission_test.cpp tests/attempt_guard_test.cpp tests/security_test.cpp tests/secret_bytes_test.cpp tests/recipient_crypto_test.cpp tests/handshake_test.cpp tests/handshake_gate_test.cpp tests/async_signaling_test.cpp tests/secure_channel_test.cpp tests/secure_attempt_test.cpp
            tests/signaling_test.cpp tests/signaling_session_test.cpp tests/signaling_candidate_test.cpp tests/signaling_flow_test.cpp
            tests/advertisement_test.cpp tests/advertisement_tracker_test.cpp
            tests/incoming_offer_resolver_test.cpp
            tests/discovery_key_test.cpp tests/discovery_window_test.cpp)
        target_include_directories(p2pdc_tests PRIVATE ${SODIUM_INCLUDE_DIR})
    endif()
    target_link_libraries(p2pdc_tests PRIVATE p2pdc::p2pdc GTest::gtest_main)
    target_compile_options(p2pdc_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>
    )
    gtest_discover_tests(p2pdc_tests)

    if(P2PDC_TEST_NATIVE_TRANSPORT)
        if(UNIX AND NOT APPLE)
            add_executable(p2pdc_browser_interop_peer EXCLUDE_FROM_ALL tests/browser_interop_peer.cpp)
            target_link_libraries(p2pdc_browser_interop_peer PRIVATE p2pdc::p2pdc LibDataChannel::LibDataChannel)
            target_compile_options(p2pdc_browser_interop_peer PRIVATE ${p2pdc_warning_flags})
            if(P2PDC_ENABLE_SODIUM)
                add_executable(p2pdc_browser_secure_interop_peer EXCLUDE_FROM_ALL tests/browser_secure_interop_peer.cpp)
                target_link_libraries(p2pdc_browser_secure_interop_peer PRIVATE p2pdc::p2pdc LibDataChannel::LibDataChannel)
                target_compile_options(p2pdc_browser_secure_interop_peer PRIVATE ${p2pdc_warning_flags})
                if(P2PDC_ENABLE_OPENDHT)
                    include(cmake/NativePublicPeerTargets.cmake)
                    add_executable(p2pdc_browser_windowed_dht_peer EXCLUDE_FROM_ALL tests/browser_windowed_dht_peer.cpp)
                    target_link_libraries(p2pdc_browser_windowed_dht_peer PRIVATE p2pdc::p2pdc ${p2pdc_opendht_target} msgpack-cxx)
                    target_compile_options(p2pdc_browser_windowed_dht_peer PRIVATE ${p2pdc_warning_flags})
                endif()
            endif()
        endif()
        add_executable(p2pdc_native_transport_tests tests/datagram_support_test.cpp tests/native_transport_test.cpp tests/native_receive_backpressure_test.cpp tests/relay_candidate_filter_test.cpp tests/native_reliable_link_test.cpp tests/native_route_readiness_test.cpp tests/native_ice_config_test.cpp tests/rtc_log_on_failure.cpp)
        if(P2PDC_ENABLE_SODIUM)
            target_sources(p2pdc_native_transport_tests PRIVATE tests/native_handshake_gate_test.cpp)
            if(P2PDC_ENABLE_OPENDHT AND P2PDC_TEST_OPENDHT)
                target_sources(p2pdc_native_transport_tests PRIVATE tests/native_dht_connection_test.cpp tests/native_attempt_test.cpp tests/native_outgoing_config_test.cpp tests/native_outgoing_public_test.cpp tests/native_incoming_listener_test.cpp tests/native_public_incoming_test.cpp tests/native_multi_connection_test.cpp tests/native_persistent_overload_test.cpp tests/native_persistent_group_test.cpp tests/native_same_consumer_test.cpp tests/node_connection_consumer.cpp tests/node_ice_validation_smoke.cpp tests/node_race_consumer.cpp tests/native_echo_example_test.cpp tests/echo_example_pair.cpp tests/echo_standalone_config_test.cpp tests/echo_protocol_test.cpp examples/echo/echo_peer.cpp examples/echo/standalone_config.cpp)
                target_link_libraries(p2pdc_native_transport_tests PRIVATE ${p2pdc_opendht_target} msgpack-cxx)
            endif()
        endif()
        target_link_libraries(p2pdc_native_transport_tests PRIVATE p2pdc::p2pdc
            LibDataChannel::LibDataChannel GTest::gtest_main)
        target_compile_options(p2pdc_native_transport_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>
        )
        gtest_discover_tests(p2pdc_native_transport_tests PROPERTIES TIMEOUT 90)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND P2PDC_ENABLE_OPENDHT AND P2PDC_TEST_OPENDHT AND P2PDC_ENABLE_SODIUM)
            add_library(p2pdc_nice_detach_probe SHARED tests/native_nice_detach_probe.cpp)
            target_link_libraries(p2pdc_nice_detach_probe PRIVATE ${CMAKE_DL_LIBS} LibDataChannel::LibDataChannel)
            add_test(NAME native_safe_nice_detach COMMAND ${CMAKE_COMMAND} -E env
                LD_PRELOAD=$<TARGET_FILE:p2pdc_nice_detach_probe>
                $<TARGET_FILE:p2pdc_native_transport_tests>
                --gtest_filter=NativeAttemptTest.ConnectsThroughDhtAndAuthenticatesMessages)
            set_tests_properties(native_safe_nice_detach PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
        endif()
        add_executable(p2pdc_native_turn_tests EXCLUDE_FROM_ALL tests/native_turn_test.cpp)
        target_link_libraries(p2pdc_native_turn_tests PRIVATE p2pdc::p2pdc
            LibDataChannel::LibDataChannel GTest::gtest_main)
        if(P2PDC_ENABLE_SODIUM AND P2PDC_ENABLE_OPENDHT)
            target_sources(p2pdc_native_turn_tests PRIVATE tests/native_turn_attempt_test.cpp tests/native_outgoing_public_test.cpp tests/native_public_incoming_test.cpp tests/native_echo_turn_test.cpp tests/echo_example_pair.cpp examples/echo/echo_peer.cpp)
            target_compile_definitions(p2pdc_native_turn_tests PRIVATE P2PDC_TEST_TURN)
            target_link_libraries(p2pdc_native_turn_tests PRIVATE ${p2pdc_opendht_target} msgpack-cxx)
        endif()
        target_compile_options(p2pdc_native_turn_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>
        )
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            add_executable(p2pdc_native_auto_turn_tests EXCLUDE_FROM_ALL tests/native_turn_test.cpp)
            target_compile_definitions(p2pdc_native_auto_turn_tests PRIVATE P2PDC_TEST_BLOCKED_DIRECT)
            target_link_libraries(p2pdc_native_auto_turn_tests PRIVATE p2pdc::p2pdc
                LibDataChannel::LibDataChannel GTest::gtest_main)
            target_compile_options(p2pdc_native_auto_turn_tests PRIVATE
                $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
        endif()
    endif()

    if(P2PDC_TEST_OPENDHT)
        add_executable(p2pdc_opendht_tests tests/opendht_test.cpp tests/opendht_replacement_test.cpp)
        target_link_libraries(p2pdc_opendht_tests PRIVATE ${p2pdc_opendht_target} msgpack-cxx GTest::gtest_main)
        if(P2PDC_ENABLE_OPENDHT)
            target_sources(p2pdc_opendht_tests PRIVATE tests/native_node_test.cpp tests/native_runner_owner_test.cpp tests/native_runner_start_test.cpp tests/native_store_test.cpp tests/native_store_lookup_test.cpp tests/native_mailbox_test.cpp tests/native_mailbox_close_test.cpp tests/native_publisher_close_test.cpp tests/native_shutdown_test.cpp)
            if(NOT EMSCRIPTEN)
                target_sources(p2pdc_opendht_tests PRIVATE tests/native_runtime_test.cpp)
            endif()
            if(NOT P2PDC_ENABLE_SODIUM)
                add_executable(p2pdc_identity_disabled_test EXCLUDE_FROM_ALL tests/native_node_identity_disabled_test.cpp)
                target_link_libraries(p2pdc_identity_disabled_test PRIVATE p2pdc::p2pdc GTest::gtest_main)
                target_compile_options(p2pdc_identity_disabled_test PRIVATE
                    $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
            endif()
        endif()
        if(P2PDC_ENABLE_SODIUM OR P2PDC_ENABLE_OPENDHT)
            target_link_libraries(p2pdc_opendht_tests PRIVATE p2pdc::p2pdc)
        endif()
        if(P2PDC_ENABLE_SODIUM)
            target_compile_definitions(p2pdc_opendht_tests PRIVATE P2PDC_TEST_SECURITY)
            if(P2PDC_ENABLE_OPENDHT)
                target_sources(p2pdc_opendht_tests PRIVATE tests/endpoint_identity_test.cpp tests/native_node_identity_test.cpp tests/native_connection_admission_test.cpp tests/native_discovery_watch_test.cpp tests/native_windowed_mailbox_test.cpp tests/native_discovery_cycle_test.cpp tests/native_public_watch_test.cpp tests/native_public_capacity_test.cpp)
                if(P2PDC_ENABLE_NATIVE_TRANSPORT)
                    target_compile_definitions(p2pdc_opendht_tests PRIVATE P2PDC_TEST_HAS_NATIVE_CONNECT)
                endif()
            endif()
        endif()
        target_compile_options(p2pdc_opendht_tests PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>
        )
        gtest_discover_tests(p2pdc_opendht_tests PROPERTIES TIMEOUT 75)
    endif()
endif()
if(P2PDC_BUILD_SHARED_TEXT)
    add_subdirectory(examples/shared-text)
endif()

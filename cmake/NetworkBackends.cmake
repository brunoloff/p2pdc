# Native network discovery backends: Nostr relays and MQTT brokers over WebSocket
# (docs/DISCOVERY_BACKENDS.md). They need libdatachannel built with WebSocket support,
# libsecp256k1 for Nostr's signatures and nlohmann/json; deps/ builds all three.
if(P2PDC_ENABLE_NATIVE_TRANSPORT AND P2PDC_ENABLE_OPENDHT AND P2PDC_ENABLE_SODIUM AND NOT ANDROID)
    set(p2pdc_default_network_backends ON)
else()
    set(p2pdc_default_network_backends OFF)
endif()
option(P2PDC_ENABLE_NETWORK_BACKENDS
    "Let native nodes meet through Nostr relays and MQTT brokers" ${p2pdc_default_network_backends})
unset(p2pdc_default_network_backends)

if(NOT P2PDC_ENABLE_NETWORK_BACKENDS)
    return()
endif()
if(NOT P2PDC_ENABLE_NATIVE_TRANSPORT OR NOT P2PDC_ENABLE_OPENDHT OR NOT P2PDC_ENABLE_SODIUM)
    message(FATAL_ERROR "P2PDC_ENABLE_NETWORK_BACKENDS needs the native transport, OpenDHT and libsodium")
endif()
get_target_property(p2pdc_rtc_definitions LibDataChannel::LibDataChannel INTERFACE_COMPILE_DEFINITIONS)
if(NOT "RTC_ENABLE_WEBSOCKET=1" IN_LIST p2pdc_rtc_definitions)
    message(FATAL_ERROR "libdatachannel was built without WebSocket support (NO_WEBSOCKET=ON). "
        "Rebuild it with the deps preset, or configure with -DP2PDC_ENABLE_NETWORK_BACKENDS=OFF")
endif()
unset(p2pdc_rtc_definitions)
if(NOT TARGET libsecp256k1::secp256k1)
    find_package(libsecp256k1 CONFIG REQUIRED)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_package(nlohmann_json 3.11 CONFIG REQUIRED)
endif()

target_sources(p2pdc PRIVATE
    src/backends/backend_loop.cpp src/backends/backend_records.cpp
    src/backends/socket_channel.cpp src/backends/schnorr.cpp
    src/backends/nostr_event.cpp src/backends/nostr_relay.cpp
    src/backends/mqtt_codec.cpp src/backends/mqtt_broker.cpp
    src/backends/watch_delivery.cpp src/backends/client_record_store.cpp
    src/backends/fan_out_store.cpp src/backends/fan_out_watch.cpp
    src/backends/network_backends.cpp)
target_compile_definitions(p2pdc PRIVATE P2PDC_HAVE_NETWORK_BACKENDS)
target_link_libraries(p2pdc PRIVATE libsecp256k1::secp256k1 nlohmann_json::nlohmann_json)

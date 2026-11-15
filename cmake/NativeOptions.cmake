# Native builds provide the public discovery/connection API by default.
# WebAssembly uses browser adapters instead of the native dependencies.
if(EMSCRIPTEN)
    if(NOT DEFINED BUILD_TESTING)
        set(BUILD_TESTING OFF CACHE BOOL "Build the testing tree")
    endif()
    set(p2pdc_default_native OFF)
else()
    set(p2pdc_default_native ON)
endif()
option(P2PDC_ENABLE_OPENDHT "Build native OpenDHT store adapter" ${p2pdc_default_native})
option(P2PDC_ENABLE_NATIVE_TRANSPORT "Build native libdatachannel adapter" ${p2pdc_default_native})
unset(p2pdc_default_native)

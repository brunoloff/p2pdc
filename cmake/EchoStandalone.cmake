# The application and library calls are identical on both targets; only the
# process/event-loop host differs.
if(P2PDC_ENABLE_SODIUM AND (EMSCRIPTEN OR (P2PDC_ENABLE_OPENDHT AND P2PDC_ENABLE_NATIVE_TRANSPORT)))
    add_executable(p2pdc_echo EXCLUDE_FROM_ALL
        examples/echo/standalone_main.cpp examples/echo/standalone_config.cpp
        examples/echo/echo_peer.cpp)
    target_link_libraries(p2pdc_echo PRIVATE p2pdc::p2pdc)
    target_compile_options(p2pdc_echo PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Werror>)
    if(EMSCRIPTEN)
        target_sources(p2pdc_echo PRIVATE examples/echo/standalone_host_browser.cpp)
        target_link_options(p2pdc_echo PRIVATE "-sNO_EXIT_RUNTIME=1"
            "SHELL:--pre-js \"${CMAKE_CURRENT_SOURCE_DIR}/examples/echo/browser_environment.js\"")
        set_property(TARGET p2pdc_echo APPEND PROPERTY LINK_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/echo/browser_environment.js")
    else()
        target_sources(p2pdc_echo PRIVATE examples/echo/standalone_host_native.cpp)
    endif()
endif()

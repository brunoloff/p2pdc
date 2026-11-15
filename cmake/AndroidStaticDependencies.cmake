# Include once from an Android parent project before adding p2pdc. Point
# pkg-config at target-only packages and request its static dependencies.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Android")
    message(FATAL_ERROR "AndroidStaticDependencies.cmake requires an Android toolchain")
endif()
if(TARGET LibDataChannel::LibDataChannel)
    message(FATAL_ERROR "LibDataChannel::LibDataChannel already exists")
endif()
if(NOT P2PDC_ANDROID_DATACHANNEL_PREFIX)
    message(FATAL_ERROR "Set P2PDC_ANDROID_DATACHANNEL_PREFIX to the patched Android installation")
endif()
foreach(p2pdc_file IN ITEMS lib/libdatachannel.a lib/libusrsctp.a include/rtc/global.hpp)
    if(NOT EXISTS "${P2PDC_ANDROID_DATACHANNEL_PREFIX}/${p2pdc_file}")
        message(FATAL_ERROR "Missing Android libdatachannel file: ${p2pdc_file}")
    endif()
endforeach()
if(NOT "--static" IN_LIST PKG_CONFIG_ARGN)
    message(FATAL_ERROR "Set PKG_CONFIG_ARGN to include --static for Android dependencies")
endif()
if("$ENV{PKG_CONFIG_LIBDIR}" STREQUAL "" OR "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
    message(FATAL_ERROR "Set PKG_CONFIG_LIBDIR and PKG_CONFIG_PATH to target-only package directories")
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(P2PDC_ANDROID_NICE REQUIRED IMPORTED_TARGET GLOBAL nice)
add_library(LibDataChannel::LibDataChannel STATIC IMPORTED GLOBAL)
set_target_properties(LibDataChannel::LibDataChannel PROPERTIES
    IMPORTED_LOCATION "${P2PDC_ANDROID_DATACHANNEL_PREFIX}/lib/libdatachannel.a"
    INTERFACE_INCLUDE_DIRECTORIES "${P2PDC_ANDROID_DATACHANNEL_PREFIX}/include"
    INTERFACE_COMPILE_DEFINITIONS "RTC_STATIC;RTC_ENABLE_MEDIA=0;RTC_ENABLE_WEBSOCKET=0")
target_link_libraries(LibDataChannel::LibDataChannel INTERFACE
    "${P2PDC_ANDROID_DATACHANNEL_PREFIX}/lib/libusrsctp.a" PkgConfig::P2PDC_ANDROID_NICE log dl)

if(EMSCRIPTEN OR NOT P2PDC_ENABLE_OPENDHT OR NOT P2PDC_ENABLE_NATIVE_TRANSPORT OR NOT P2PDC_ENABLE_SODIUM)
    message(FATAL_ERROR "The Yrs reference test needs the complete native backend")
endif()
find_package(GTest REQUIRED)
find_package(LibDataChannel CONFIG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(P2PDC_YRS_OPENDHT REQUIRED IMPORTED_TARGET opendht)
find_package(msgpack-cxx CONFIG REQUIRED)
add_executable(p2pdc_yrs_reference
    "${P2PDC_SOURCE_DIR}/integrations/yrs/reference_document.cpp"
    "${P2PDC_SOURCE_DIR}/integrations/yrs/reference_test.cpp")
target_include_directories(p2pdc_yrs_reference PRIVATE "${P2PDC_YRS_INCLUDE_DIR}")
target_link_libraries(p2pdc_yrs_reference PRIVATE p2pdc::p2pdc GTest::gtest_main
    PkgConfig::P2PDC_YRS_OPENDHT msgpack-cxx LibDataChannel::LibDataChannel "${P2PDC_YRS_LIBRARY}")
target_compile_options(p2pdc_yrs_reference PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Werror>)

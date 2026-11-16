find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
find_path(P2PDC_YRS_INCLUDE_DIR libyrs.h HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.deps/y-crdt/tests-ffi/include")
find_library(P2PDC_YRS_LIBRARY NAMES yrs HINTS "${CMAKE_CURRENT_SOURCE_DIR}/../../.deps/y-crdt-target/debug")
if(NOT P2PDC_YRS_INCLUDE_DIR OR NOT P2PDC_YRS_LIBRARY)
    message(FATAL_ERROR "The shared text example needs target-compatible Yrs headers and library. Set P2PDC_YRS_INCLUDE_DIR and P2PDC_YRS_LIBRARY, or use -DP2PDC_BUILD_SHARED_TEXT=OFF.")
endif()
if(NOT P2PDC_ENABLE_SODIUM OR (NOT EMSCRIPTEN AND (NOT P2PDC_ENABLE_OPENDHT OR NOT P2PDC_ENABLE_NATIVE_TRANSPORT)))
    message(FATAL_ERROR "The shared text example needs the full p2pdc discovery, security and transport backends. Disable it with -DP2PDC_BUILD_SHARED_TEXT=OFF for partial builds.")
endif()
qt_add_executable(p2pdc_shared_text main.cpp session.cpp session_discovery.cpp
    session_connection.cpp session_cursor.cpp cursor.cpp document.cpp session.hpp document.hpp)
set_target_properties(p2pdc_shared_text PROPERTIES AUTOMOC ON)
set_source_files_properties(Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
qt_add_qml_module(p2pdc_shared_text URI SharedText VERSION 1.0 RESOURCE_PREFIX /
    QML_FILES Main.qml)
target_include_directories(p2pdc_shared_text SYSTEM PRIVATE "${P2PDC_YRS_INCLUDE_DIR}" "${SODIUM_INCLUDE_DIR}")
target_link_libraries(p2pdc_shared_text PRIVATE p2pdc::p2pdc Qt6::Core Qt6::Gui Qt6::Qml
    Qt6::Quick Qt6::QuickControls2 "${P2PDC_YRS_LIBRARY}" "${SODIUM_LIBRARY}")
target_compile_options(p2pdc_shared_text PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_executable(p2pdc_shared_text_tests document_test.cpp cursor_test.cpp sync_state_test.cpp document.cpp cursor.cpp)
    target_include_directories(p2pdc_shared_text_tests SYSTEM PRIVATE "${P2PDC_YRS_INCLUDE_DIR}")
    target_link_libraries(p2pdc_shared_text_tests PRIVATE p2pdc::p2pdc Qt6::Core
        GTest::gtest_main "${P2PDC_YRS_LIBRARY}")
    target_compile_options(p2pdc_shared_text_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
    include(GoogleTest)
    gtest_discover_tests(p2pdc_shared_text_tests)
endif()
if(BUILD_TESTING AND NOT EMSCRIPTEN)
    add_executable(p2pdc_shared_text_session_tests session_test.cpp session.cpp session_discovery.cpp
        session_connection.cpp session_cursor.cpp cursor.cpp document.cpp session.hpp)
    set_target_properties(p2pdc_shared_text_session_tests PROPERTIES AUTOMOC ON)
    target_include_directories(p2pdc_shared_text_session_tests SYSTEM PRIVATE "${P2PDC_YRS_INCLUDE_DIR}" "${SODIUM_INCLUDE_DIR}")
    target_link_libraries(p2pdc_shared_text_session_tests PRIVATE p2pdc::p2pdc Qt6::Core Qt6::Qml GTest::gtest
        "${P2PDC_YRS_LIBRARY}" "${SODIUM_LIBRARY}" ${p2pdc_opendht_target} msgpack-cxx)
    target_compile_options(p2pdc_shared_text_session_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
    gtest_discover_tests(p2pdc_shared_text_session_tests PROPERTIES TIMEOUT 90)
    add_executable(p2pdc_shared_text_ui_tests ui_test.cpp session.cpp session_discovery.cpp
        session_connection.cpp session_cursor.cpp cursor.cpp document.cpp session.hpp)
    set_target_properties(p2pdc_shared_text_ui_tests PROPERTIES AUTOMOC ON)
    target_include_directories(p2pdc_shared_text_ui_tests SYSTEM PRIVATE "${P2PDC_YRS_INCLUDE_DIR}" "${SODIUM_INCLUDE_DIR}")
    target_compile_definitions(p2pdc_shared_text_ui_tests PRIVATE
        P2PDC_SHARED_TEXT_QML="${CMAKE_CURRENT_SOURCE_DIR}/Main.qml")
    target_link_libraries(p2pdc_shared_text_ui_tests PRIVATE p2pdc::p2pdc Qt6::Core Qt6::Gui
        Qt6::Qml Qt6::Quick Qt6::QuickControls2 GTest::gtest "${P2PDC_YRS_LIBRARY}" "${SODIUM_LIBRARY}")
    target_compile_options(p2pdc_shared_text_ui_tests PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang>:${p2pdc_warning_flags}>)
    gtest_discover_tests(p2pdc_shared_text_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
endif()

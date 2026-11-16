# Warning and hardening flags for this project's own targets.
#
# -Werror is useful while developing p2pdc but breaks consumers whose newer
# compilers add warnings, so it defaults to ON only for top-level builds.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(p2pdc_is_top_level ON)
else()
    set(p2pdc_is_top_level OFF)
endif()
option(P2PDC_WERROR "Treat compiler warnings as errors in p2pdc targets" ${p2pdc_is_top_level})
option(P2PDC_HARDENING "Build the p2pdc library with compiler and linker hardening" ON)

set(p2pdc_warning_flags -Wall -Wextra -Wpedantic)
if(P2PDC_WERROR)
    list(APPEND p2pdc_warning_flags -Werror)
endif()

# Applies hardening that does not depend on the host toolchain's defaults:
# stack protector, stack-clash protection, libstdc++ assertions, PIC, and on
# optimized builds _FORTIFY_SOURCE (it needs optimization to take effect). ELF
# targets also get full RELRO and immediate binding when linked as shared.
function(p2pdc_apply_hardening target)
    if(NOT P2PDC_HARDENING OR EMSCRIPTEN OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        return()
    endif()
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_options(${target} PRIVATE -fstack-protector-strong -fstack-clash-protection
        $<$<CONFIG:Release,RelWithDebInfo,MinSizeRel>:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>)
    target_compile_definitions(${target} PRIVATE _GLIBCXX_ASSERTIONS)
    if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
        target_link_options(${target} PRIVATE -Wl,-z,relro -Wl,-z,now)
    endif()
endfunction()

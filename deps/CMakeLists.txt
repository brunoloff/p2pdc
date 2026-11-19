# Downloads and builds the libraries p2pdc needs, at pinned versions, into
# CMAKE_INSTALL_PREFIX. Nothing is copied into this repository. Use the "deps" and "deps-web"
# presets, or configure p2pdc with -DP2PDC_BUILD_DEPENDENCIES=ON from a parent project.
cmake_minimum_required(VERSION 3.25)
project(p2pdc_dependencies LANGUAGES C CXX)

include(ExternalProject)
include(ProcessorCount)

set(patches "${CMAKE_CURRENT_LIST_DIR}/../patches")
set(prefix "${CMAKE_INSTALL_PREFIX}")
if(NOT prefix OR CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    message(FATAL_ERROR "Set CMAKE_INSTALL_PREFIX to the folder that receives the dependencies")
endif()
if(DEFINED ENV{CMAKE_BUILD_PARALLEL_LEVEL})
    set(jobs "$ENV{CMAKE_BUILD_PARALLEL_LEVEL}")
else()
    ProcessorCount(jobs)
endif()

set(sodium_url "https://download.libsodium.org/libsodium/releases/libsodium-1.0.22.tar.gz")
set(sodium_hash adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349)

if(EMSCRIPTEN)
    # Browsers provide WebRTC themselves, so libsodium is the only library to build.
    ExternalProject_Add(libsodium
        URL "${sodium_url}" URL_HASH SHA256=${sodium_hash}
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CFLAGS=-O1
            emconfigure <SOURCE_DIR>/configure --host=wasm32-unknown-emscripten
            --prefix=${prefix} --disable-shared --enable-static --disable-asm --disable-pie
            --without-pthreads
        BUILD_COMMAND emmake make -j ${jobs}
        INSTALL_COMMAND emmake make -j ${jobs} install)
    return()
endif()

# Every library finds the others beside it, wherever the folder ends up.
set(cmake_arguments
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${prefix}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_PREFIX_PATH=${prefix}
    "-DCMAKE_INSTALL_RPATH=\$ORIGIN"
    -DBUILD_SHARED_LIBS=ON)
set(opendht_dependencies)
set(libnice_dependencies)
set(libdatachannel_dependencies)
set(build_environment)
if(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/macos.cmake")
endif()

ExternalProject_Add(libsodium
    URL "${sodium_url}" URL_HASH SHA256=${sodium_hash}
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ${build_environment} <SOURCE_DIR>/configure --prefix=${prefix}
        --libdir=${prefix}/lib
    BUILD_COMMAND ${build_environment} make -j ${jobs}
    INSTALL_COMMAND ${build_environment} make -j ${jobs} install)

ExternalProject_Add(msgpack_cxx
    URL "https://github.com/msgpack/msgpack-c/releases/download/cpp-8.0.0/msgpack-cxx-8.0.0.tar.gz"
    URL_HASH SHA256=4a3c0c0ac55ef4456c2d0b93c21b5d105aa3a8f21ef8fa9758550feaf989b92f
    CMAKE_ARGS ${cmake_arguments} -DMSGPACK_USE_BOOST=OFF -DMSGPACK_BUILD_TESTS=OFF
        -DMSGPACK_BUILD_EXAMPLES=OFF -DMSGPACK_BUILD_DOCS=OFF)

ExternalProject_Add(opendht
    DEPENDS msgpack_cxx ${opendht_dependencies}
    GIT_REPOSITORY https://github.com/savoirfairelinux/opendht.git
    GIT_TAG 621b44c8e807e9d3b98794f6b5f8101ad216fd8f # v4.4.1
    UPDATE_DISCONNECTED TRUE
    CMAKE_ARGS ${cmake_arguments} -DBUILD_TESTING=OFF -DOPENDHT_USE_PKGCONFIG=ON
        -DOPENDHT_HTTP=OFF -DOPENDHT_PEER_DISCOVERY=OFF -DOPENDHT_PROXY_CLIENT=OFF
        -DOPENDHT_PROXY_SERVER=OFF -DOPENDHT_TOOLS=OFF -DOPENDHT_EXAMPLES=OFF
        -DOPENDHT_PYTHON=OFF -DOPENDHT_C=OFF -DOPENDHT_DOWNLOAD_DEPS=OFF)

ExternalProject_Add(libnice
    DEPENDS ${libnice_dependencies}
    GIT_REPOSITORY https://github.com/libnice/libnice.git
    GIT_TAG af62014d520ac09c8fe1974bda1b253114c68843 # 0.1.24
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND git apply "${patches}/libnice-v0.1.24-p2pdc-tls.patch"
    CONFIGURE_COMMAND ${build_environment} meson setup <BINARY_DIR> <SOURCE_DIR>
        --prefix=${prefix} --libdir=lib
        --buildtype=release -Dcrypto-library=openssl -Dgupnp=disabled -Dgstreamer=disabled
        -Dintrospection=disabled -Dtests=disabled -Dexamples=disabled
    BUILD_COMMAND ${build_environment} meson compile -C <BINARY_DIR> -j ${jobs}
    INSTALL_COMMAND ${build_environment} meson install -C <BINARY_DIR> --no-rebuild)

ExternalProject_Add(libdatachannel
    DEPENDS libnice ${libdatachannel_dependencies}
    GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
    GIT_TAG 9e6a13abbb6846c003d817d0387b6706466e2b03 # v0.23.2
    GIT_SUBMODULES_RECURSE TRUE
    UPDATE_DISCONNECTED TRUE
    PATCH_COMMAND git apply "${patches}/libdatachannel-v0.23.2-p2pdc.patch"
    CMAKE_ARGS ${cmake_arguments} -DUSE_NICE=ON -DNO_MEDIA=ON -DNO_WEBSOCKET=ON
        -DNO_EXAMPLES=ON -DNO_TESTS=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target datachannel)

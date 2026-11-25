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
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
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
if(APPLE)
    set(beside_library @loader_path)
else()
    set(beside_library "\$ORIGIN")
endif()
set(cmake_arguments
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=${prefix}
    -DCMAKE_INSTALL_LIBDIR=lib
    -DCMAKE_PREFIX_PATH=${prefix}
    "-DCMAKE_INSTALL_RPATH=${beside_library}"
    -DBUILD_SHARED_LIBS=ON)
set(opendht_dependencies)
set(opendht_patch)
set(libnice_dependencies)
set(libdatachannel_dependencies)
set(build_environment)
set(configure_shell)
if(WIN32)
    # Ninja runs commands through cmd.exe, which cannot run shell scripts such as configure.
    set(configure_shell sh)
    # MSYS2 has jsoncpp, which makes OpenDHT require simdutf; MSYS2 does not package that.
    ExternalProject_Add(simdutf
        URL https://github.com/simdutf/simdutf/archive/refs/tags/v8.0.0.tar.gz
        URL_HASH SHA256=a21c34c52d91a229591e4ebc8822a876604cf2fffeac9ec065bfda7cbfb9d680
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CMAKE_ARGS ${cmake_arguments} -DSIMDUTF_TESTS=OFF -DSIMDUTF_TOOLS=OFF
            -DSIMDUTF_ICONV=OFF)
    set(opendht_dependencies simdutf)
    set(opendht_patch PATCH_COMMAND ${CMAKE_COMMAND} -P
        "${CMAKE_CURRENT_LIST_DIR}/opendht-skip-runtime-dependencies.cmake")
endif()
if(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/macos.cmake")
endif()

ExternalProject_Add(libsodium
    URL "${sodium_url}" URL_HASH SHA256=${sodium_hash}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ${build_environment} ${configure_shell} <SOURCE_DIR>/configure --prefix=${prefix}
        --libdir=${prefix}/lib
    BUILD_COMMAND ${build_environment} make -j ${jobs}
    INSTALL_COMMAND ${build_environment} make -j ${jobs} install)

ExternalProject_Add(msgpack_cxx
    URL "https://github.com/msgpack/msgpack-c/releases/download/cpp-8.0.0/msgpack-cxx-8.0.0.tar.gz"
    URL_HASH SHA256=4a3c0c0ac55ef4456c2d0b93c21b5d105aa3a8f21ef8fa9758550feaf989b92f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS ${cmake_arguments} -DMSGPACK_USE_BOOST=OFF -DMSGPACK_BUILD_TESTS=OFF
        -DMSGPACK_BUILD_EXAMPLES=OFF -DMSGPACK_BUILD_DOCS=OFF)

ExternalProject_Add(opendht
    DEPENDS msgpack_cxx ${opendht_dependencies}
    GIT_REPOSITORY https://github.com/savoirfairelinux/opendht.git
    GIT_TAG 621b44c8e807e9d3b98794f6b5f8101ad216fd8f # v4.4.1
    UPDATE_DISCONNECTED TRUE
    ${opendht_patch}
    CMAKE_ARGS ${cmake_arguments} -DBUILD_TESTING=OFF -DOPENDHT_USE_PKGCONFIG=ON
        -DOPENDHT_HTTP=OFF -DOPENDHT_PEER_DISCOVERY=OFF -DOPENDHT_PROXY_CLIENT=OFF
        -DOPENDHT_PROXY_SERVER=OFF -DOPENDHT_TOOLS=OFF -DOPENDHT_EXAMPLES=OFF
        -DOPENDHT_PYTHON=OFF -DOPENDHT_C=OFF -DOPENDHT_DOWNLOAD_DEPS=OFF -DOPENDHT_DOCUMENTATION=OFF)
if(WIN32)
    # OpenDHT names its import library and places its DLL in ways MinGW's linker and Windows'
    # loader do not look for; add copies under the usual names.
    ExternalProject_Add_Step(opendht windows_names DEPENDEES install
        COMMAND ${CMAKE_COMMAND} -E copy ${prefix}/lib/libopendht_import.lib
            ${prefix}/lib/libopendht.dll.a
        COMMAND ${CMAKE_COMMAND} -E make_directory ${prefix}/bin
        COMMAND ${CMAKE_COMMAND} -E copy ${prefix}/lib/libopendht.dll ${prefix}/bin)
endif()

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
    # WebSocket carries the Nostr and MQTT discovery backends (docs/DISCOVERY_BACKENDS.md).
    CMAKE_ARGS ${cmake_arguments} -DUSE_NICE=ON -DNO_MEDIA=ON -DNO_WEBSOCKET=OFF
        -DNO_EXAMPLES=ON -DNO_TESTS=ON
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target datachannel)

# BIP340 Schnorr signatures for Nostr events (MIT).
ExternalProject_Add(secp256k1
    URL https://github.com/bitcoin-core/secp256k1/archive/refs/tags/v0.7.0.tar.gz
    URL_HASH SHA256=073d19064f3600014750d6949b31a0c957aa7b98920fb4aaa495be07e8e7cd00
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS ${cmake_arguments} -DSECP256K1_ENABLE_MODULE_SCHNORRSIG=ON
        -DSECP256K1_ENABLE_MODULE_EXTRAKEYS=ON -DSECP256K1_ENABLE_MODULE_ECDH=OFF
        -DSECP256K1_ENABLE_MODULE_RECOVERY=OFF -DSECP256K1_ENABLE_MODULE_ELLSWIFT=OFF
        -DSECP256K1_ENABLE_MODULE_MUSIG=OFF -DSECP256K1_BUILD_TESTS=OFF
        -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF -DSECP256K1_BUILD_BENCHMARK=OFF
        -DSECP256K1_BUILD_EXAMPLES=OFF)

# JSON for the Nostr and PeerJS protocols (MIT; header only).
ExternalProject_Add(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    CMAKE_ARGS ${cmake_arguments} -DJSON_BuildTests=OFF -DJSON_Install=ON)

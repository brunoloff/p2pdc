# Libraries that Linux takes from the system but macOS must build, so that everything shipped
# runs on macOS 13 and later (Homebrew's builds target the newest macOS). Included by
# deps/CMakeLists.txt, which provides `prefix`, `jobs` and `cmake_arguments`.
set(macos_target 13.0)
# Only this prefix's libraries count, not Homebrew's.
set(macos_environment ${CMAKE_COMMAND} -E env MACOSX_DEPLOYMENT_TARGET=${macos_target}
    PKG_CONFIG_LIBDIR=${prefix}/lib/pkgconfig "CPPFLAGS=-I${prefix}/include"
    "LDFLAGS=-L${prefix}/lib -Wl,-headerpad_max_install_names")
list(APPEND cmake_arguments -DCMAKE_OSX_DEPLOYMENT_TARGET=${macos_target}
    "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local" -DOPENSSL_ROOT_DIR=${prefix}
    "-DCMAKE_INSTALL_NAME_DIR=${prefix}/lib")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(openssl_target darwin64-arm64-cc)
else()
    set(openssl_target darwin64-x86_64-cc)
endif()

# p2pdc_autotools(NAME URL SHA256 [DEPENDS ...] [OPTIONS ...]): configure, make, make install.
function(p2pdc_autotools name url hash)
    cmake_parse_arguments(PARSE_ARGV 3 arg "" "" "DEPENDS;OPTIONS")
    ExternalProject_Add(${name}
        URL "${url}" URL_HASH SHA256=${hash}
        DEPENDS ${arg_DEPENDS}
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND ${macos_environment} <SOURCE_DIR>/configure --prefix=${prefix}
            --libdir=${prefix}/lib --enable-shared --disable-static ${arg_OPTIONS}
        BUILD_COMMAND ${macos_environment} make -j ${jobs}
        INSTALL_COMMAND ${macos_environment} make install)
endfunction()

p2pdc_autotools(gmp https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz
    a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898)
p2pdc_autotools(nettle https://ftp.gnu.org/gnu/nettle/nettle-3.10.2.tar.gz
    fe9ff51cb1f2abb5e65a6b8c10a92da0ab5ab6eaf26e7fc2b675c45f1fb519b5
    DEPENDS gmp OPTIONS --disable-documentation)
p2pdc_autotools(libtasn1 https://ftp.gnu.org/gnu/libtasn1/libtasn1-4.20.0.tar.gz
    92e0e3bd4c02d4aeee76036b2ddd83f0c732ba4cda5cb71d583272b23587a76c
    OPTIONS --disable-doc)
p2pdc_autotools(gnutls https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/gnutls-3.8.9.tar.xz
    69e113d802d1670c4d5ac1b99040b1f2d5c7c05daec5003813c049b5184820ed
    DEPENDS nettle libtasn1
    OPTIONS --without-p11-kit --with-included-unistring --without-idn --without-zstd
        --without-brotli --without-tpm --without-tpm2 --without-leancrypto --disable-doc
        --disable-tools --disable-tests --disable-cxx --disable-nls)

ExternalProject_Add(argon2
    URL https://github.com/P-H-C/phc-winner-argon2/archive/refs/tags/20190702.tar.gz
    URL_HASH SHA256=daf972a89577f8772602bf2eb38b6a3dd3d922bf5724d45e7f9589b5e830442c
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ${macos_environment} make -j ${jobs} PREFIX=${prefix} LIBRARY_REL=lib
        OPTTARGET=none
    INSTALL_COMMAND ${macos_environment} make install PREFIX=${prefix} LIBRARY_REL=lib
        OPTTARGET=none)

ExternalProject_Add(fmt
    URL https://github.com/fmtlib/fmt/releases/download/11.2.0/fmt-11.2.0.zip
    URL_HASH SHA256=203eb4e8aa0d746c62d8f903df58e0419e3751591bb53ff971096eaa0ebd4ec3
    CMAKE_ARGS ${cmake_arguments} -DFMT_TEST=OFF -DFMT_DOC=OFF)

ExternalProject_Add(openssl
    URL https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz
    URL_HASH SHA256=967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99
    BUILD_IN_SOURCE TRUE
    CONFIGURE_COMMAND ${macos_environment} <SOURCE_DIR>/Configure ${openssl_target} shared
        no-tests no-docs --prefix=${prefix} --libdir=lib
    BUILD_COMMAND ${macos_environment} make -j ${jobs}
    INSTALL_COMMAND ${macos_environment} make install_sw)

# GLib fetches what it needs and the system lacks (such as PCRE2 and libintl) itself.
ExternalProject_Add(glib
    URL https://download.gnome.org/sources/glib/2.84/glib-2.84.4.tar.xz
    URL_HASH SHA256=8a9ea10943c36fc117e253f80c91e477b673525ae45762942858aef57631bb90
    CONFIGURE_COMMAND ${macos_environment} meson setup <BINARY_DIR> <SOURCE_DIR>
        --prefix=${prefix} --libdir=lib --buildtype=release --wrap-mode=forcefallback
        --force-fallback-for=libintl,pcre2 -Dtests=false -Dintrospection=disabled -Dnls=disabled
        -Ddtrace=disabled -Dsystemtap=disabled -Dman-pages=disabled -Ddocumentation=false
    BUILD_COMMAND ${macos_environment} meson compile -C <BINARY_DIR> -j ${jobs}
    INSTALL_COMMAND ${macos_environment} meson install -C <BINARY_DIR> --no-rebuild)

set(opendht_dependencies gnutls argon2 fmt)
set(libnice_dependencies glib openssl)
set(libdatachannel_dependencies openssl)
set(build_environment ${macos_environment})

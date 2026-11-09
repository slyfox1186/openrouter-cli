cmake_minimum_required(VERSION 4.0)
project(openrouter_cli VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckPIESupported)
check_pie_supported()

option(ORCLI_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ORCLI_NATIVE "Optimize release builds for the CPU of the building machine" ON)
include(CTest)
include(FetchContent)

# ---------------------------------------------------------------------------------------------
# Third-party code: release archives pinned by SHA-256 so a moved tag or tampered download fails
# the build. Everything that ends up in the binary is linked statically; only glibc stays shared,
# because a statically linked glibc cannot resolve host names reliably on other systems.
# ---------------------------------------------------------------------------------------------

FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    EXCLUDE_FROM_ALL SYSTEM)

set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cli11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
    URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
    EXCLUDE_FROM_ALL SYSTEM)

# Ubuntu's static libcrypto.a calls into jitterentropy, whose static library is not installed;
# 3.6.3 matches the version Ubuntu's OpenSSL was built against. Its own CMake compiles it -O0,
# which the library requires for entropy quality.
set(STACK_PROTECTOR ON CACHE BOOL "" FORCE)
set(INTERNAL_TIMER ON CACHE BOOL "" FORCE)
FetchContent_Declare(jitterentropy
    URL https://github.com/smuellerDD/jitterentropy-library/archive/refs/tags/v3.6.3.tar.gz
    URL_HASH SHA256=9b3819e275e6473a94c70cfaae7cc2a87a11f60b862e694745daa5d232b0eea3
    EXCLUDE_FROM_ALL SYSTEM)

FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz
    URL_HASH SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
    EXCLUDE_FROM_ALL SYSTEM)

# Dependencies build static libraries. BUILD_TESTING is a normal variable here so curl's and
# jitterentropy's own test suites stay off while ours keeps its cache value.
set(_orcli_build_testing ${BUILD_TESTING})
set(BUILD_TESTING OFF)
set(BUILD_SHARED_LIBS OFF)

FetchContent_MakeAvailable(nlohmann_json cli11)
# jitterentropy 3.6.3 requests CMake 2.8.12 policies, which CMake 4 rejects. A 3.10 floor
# also avoids the deprecation warning. Keep higher caller floors and restore the prior value.
block(SCOPE_FOR VARIABLES)
    if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM OR CMAKE_POLICY_VERSION_MINIMUM VERSION_LESS 3.10)
        set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
    endif()
    FetchContent_MakeAvailable(jitterentropy)
endblock()

set(OPENSSL_USE_STATIC_LIBS TRUE)
find_package(OpenSSL 3 REQUIRED)
find_library(ORCLI_ZLIB_STATIC NAMES libz.a REQUIRED)
find_library(ORCLI_ZSTD_STATIC NAMES libzstd.a REQUIRED)
find_package(Threads REQUIRED)
# What `pkg-config --static --libs libcrypto` lists on Ubuntu 26.04, with jitterentropy built above.
set_property(TARGET OpenSSL::Crypto APPEND PROPERTY INTERFACE_LINK_LIBRARIES
    jitterentropy ${ORCLI_ZLIB_STATIC} ${ORCLI_ZSTD_STATIC} ${CMAKE_DL_LIBS} Threads::Threads)

# HTTP is needed by the offline mock server; HTTPS serves the API.
# Disable other protocols and unused optional libraries.
set(BUILD_STATIC_LIBS ON)
set(BUILD_CURL_EXE OFF)
set(BUILD_EXAMPLES OFF)
set(BUILD_LIBCURL_DOCS OFF)
set(BUILD_MISC_DOCS OFF)
set(ENABLE_CURL_MANUAL OFF)
set(CURL_ENABLE_EXPORT_TARGET OFF)
set(CURL_DISABLE_INSTALL ON)
set(PICKY_COMPILER OFF)
set(CURL_USE_OPENSSL ON)
set(CURL_USE_LIBPSL OFF)
set(CURL_USE_LIBSSH2 OFF)
set(CURL_USE_LIBSSH OFF)
set(CURL_USE_GSSAPI OFF)
set(USE_LIBIDN2 OFF)
set(USE_NGHTTP2 OFF)
set(USE_NGTCP2 OFF)
# curl's tri-state cache defaults can replace normal variables on a fresh configure.
set(CURL_ZLIB OFF CACHE STRING "Build curl without zlib compression" FORCE)
set(CURL_BROTLI OFF CACHE STRING "Build curl without Brotli compression" FORCE)
set(CURL_ZSTD OFF CACHE STRING "Build curl without zstd compression" FORCE)
set(CURL_DISABLE_LDAP ON)
set(CURL_DISABLE_DICT ON)
set(CURL_DISABLE_FILE ON)
set(CURL_DISABLE_FTP ON)
set(CURL_DISABLE_GOPHER ON)
set(CURL_DISABLE_IMAP ON)
# IPFS/IPNS are curl-tool features; disable them so the summary matches our libcurl build.
set(CURL_DISABLE_IPFS ON)
set(CURL_DISABLE_MQTT ON)
set(CURL_DISABLE_POP3 ON)
set(CURL_DISABLE_RTSP ON)
set(CURL_DISABLE_SMB ON)
set(CURL_DISABLE_SMTP ON)
set(CURL_DISABLE_TELNET ON)
set(CURL_DISABLE_TFTP ON)
set(CURL_DISABLE_WEBSOCKETS ON)
FetchContent_MakeAvailable(curl)

set(BUILD_TESTING ${_orcli_build_testing})

# ---------------------------------------------------------------------------------------------
# Flags for first-party code: OpenSSF "Compiler Options Hardening Guide for C and C++"
# (TL;DR set, plus the x86_64 and production-code additions), warnings as errors.
# ---------------------------------------------------------------------------------------------

add_library(orcli_flags INTERFACE)
target_compile_options(orcli_flags INTERFACE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat -Wformat=2
    -Wimplicit-fallthrough -Werror=format-security -Werror
    -fstrict-flex-arrays=3 -fstack-clash-protection -fstack-protector-strong
    -fcf-protection=full -fzero-call-used-regs=used-gpr
    -fno-delete-null-pointer-checks -fno-strict-overflow -fno-strict-aliasing
    -ftrivial-auto-var-init=zero
    $<$<CONFIG:Release>:-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3>
    $<$<AND:$<CONFIG:Release>,$<BOOL:${ORCLI_NATIVE}>>:-march=native>)
target_compile_definitions(orcli_flags INTERFACE _GLIBCXX_ASSERTIONS)
target_link_options(orcli_flags INTERFACE
    -Wl,-z,nodlopen -Wl,-z,noexecstack -Wl,-z,relro -Wl,-z,now
    -Wl,--as-needed -Wl,--no-copy-dt-needed-entries)
if(ORCLI_SANITIZE)
    target_compile_options(orcli_flags INTERFACE
        -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    target_link_options(orcli_flags INTERFACE -fsanitize=address,undefined)
endif()

add_library(orcli_core STATIC
    src/account.cpp
    src/account_commands.cpp
    src/auth.cpp
    src/auth_commands.cpp
    src/commands.cpp
    src/config.cpp
    src/credentials.cpp
    src/context.cpp
    src/costlog.cpp
    src/fsutil.cpp
    src/http.cpp
    src/models.cpp
    src/request.cpp
    src/secrets.cpp
    src/session.cpp
    src/sse.cpp)
target_include_directories(orcli_core PUBLIC src)
# OpenSSL::Crypto is linked directly because auth.cpp (RAND_bytes, EVP SHA-256) and
# credentials.cpp (OPENSSL_cleanse) call it, not only libcurl.
target_link_libraries(orcli_core PUBLIC
    nlohmann_json::nlohmann_json CURL::libcurl OpenSSL::Crypto orcli_flags)
# The project directory is compiled in so the binary finds .env and state/ from any directory.
target_compile_definitions(orcli_core PRIVATE ORCLI_DEFAULT_HOME="${CMAKE_SOURCE_DIR}")
# Test builds may swap systemd-creds for a fake (OPENROUTER_CLI_TEST_CREDS_TOOL); the release
# preset turns BUILD_TESTING off, so the installed binary never honours that variable.
if(BUILD_TESTING)
    target_compile_definitions(orcli_core PRIVATE ORCLI_TEST_HOOKS)
endif()

# The help formatter is its own library so the unit tests can render pages without main.cpp.
add_library(orcli_help STATIC src/help.cpp)
target_include_directories(orcli_help PUBLIC src)
target_link_libraries(orcli_help PUBLIC CLI11::CLI11 orcli_flags)

add_executable(openrouter-cli src/main.cpp)
target_link_libraries(openrouter-cli PRIVATE orcli_core orcli_help CLI11::CLI11 orcli_flags)
target_link_options(openrouter-cli PRIVATE -static-libstdc++ -static-libgcc)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set_property(TARGET orcli_core orcli_help openrouter-cli
        PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
endif()

if(BUILD_TESTING)
    # Header-only test dependencies: SOURCE_SUBDIR names a missing directory so their own CMake
    # projects (tests, examples, optional TLS and compression probes) are never configured.
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
        SOURCE_SUBDIR no-cmake)
    FetchContent_Declare(httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.63.0.tar.gz
        URL_HASH SHA256=44ab42e16926e95f7641d286bc90d739ab1c16886114d7cc5bc7a9f905c5edf8
        SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(doctest httplib)
    add_subdirectory(tests)
endif()

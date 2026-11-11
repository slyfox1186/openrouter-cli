cmake_minimum_required(VERSION 4.0)
project(openrouter_cli VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(CheckPIESupported)
check_pie_supported()

option(ORCLI_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(ORCLI_NATIVE "Optimize release builds for the CPU of the building machine" OFF)
include(CTest)
include(FetchContent)

# Check the supported platform and actual facilities before downloading dependencies.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    message(FATAL_ERROR "openrouter-cli supports Linux x86-64 with glibc")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "openrouter-cli requires Clang for C and C++")
endif()
include(CheckCXXSourceCompiles)
check_cxx_source_compiles([[
    #include <features.h>
    #ifndef __GLIBC__
    #error "glibc is required"
    #endif
    #include <chrono>
    #include <expected>
    #include <flat_map>
    #include <format>
    #include <functional>
    #include <numeric>
    #include <print>
    #include <ranges>
    #include <span>
    #include <sstream>
    #include <string>
    #include <thread>
    #include <vector>
    struct Noncopyable { Noncopyable(const Noncopyable&) = delete("not copyable"); };
    int main() {
        std::expected<int, int> value{1};
        auto mapped = value.transform([](int n) { return n + 1; });
        std::flat_map<int, int> table;
        table[1] = *mapped;
        const std::vector<int> values{1, 2};
        auto copy = values | std::ranges::to<std::vector<int>>();
        for (const auto [index, element] : copy | std::views::enumerate) {
            table[static_cast<int>(index)] = element;
        }
        for (const auto [left, right] : std::views::zip(values, copy)) {
            table[left] = right;
        }
        const std::vector<std::string> words{"one", "two"};
        auto joined = words | std::views::join_with(' ') | std::ranges::to<std::string>();
        std::chrono::sys_seconds timestamp{};
        std::istringstream input{"2026-01-01T00:00:00Z"};
        input >> std::chrono::parse("%FT%TZ", timestamp);
        std::println("{} {:%FT%TZ}", joined, timestamp);
        std::move_only_function<int()> function = [] { return 1; };
        std::jthread worker([] {});
        const std::span<const int> view{values};
        std::println("{}", std::format("{}", function() + view.front()));
    #if __cpp_lib_saturation_arithmetic >= 202603L
        return std::saturating_add(0, table[1]);
    #elif defined(__cpp_lib_saturation_arithmetic)
        return std::add_sat(0, table[1]);
    #else
    #error "saturation arithmetic is required"
    #endif
    }
]] ORCLI_HAS_REQUIRED_CXX26)
if(NOT ORCLI_HAS_REQUIRED_CXX26)
    message(FATAL_ERROR "The selected compiler/standard library lacks required C++26 facilities; see CMakeFiles/CMakeConfigureLog.yaml")
endif()

set(CMAKE_SKIP_RPATH TRUE)
set(OPENSSL_USE_STATIC_LIBS FALSE)
find_package(OpenSSL 3...<5 REQUIRED COMPONENTS SSL Crypto)
foreach(_library IN ITEMS OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY)
    if(NOT "${${_library}}" MATCHES "\\.so(\\.[0-9]+)*$")
        message(FATAL_ERROR "Shared system OpenSSL is required, but ${_library} resolved to '${${_library}}' in '${CMAKE_BINARY_DIR}'. For an existing build directory, rerun the same scripts/build.sh command with --fresh added. If that does not help, install the shared OpenSSL 3 or 4 development libraries and check the OpenSSL search paths.")
    endif()
endforeach()
find_package(Threads REQUIRED)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/openssl-libraries.txt"
    CONTENT "$<TARGET_FILE:OpenSSL::SSL>\n$<TARGET_FILE:OpenSSL::Crypto>\n")

set(ORCLI_DOWNLOAD_USER_AGENT "Mozilla/5.0 (X11; Linux x86_64; rv:159.0) Gecko/20100101 Firefox/159.0")
set(CMAKE_TLS_VERIFY TRUE)

# HTTP_HEADER is a list. Keep the reference literal until the generated download script
# expands it inside one quoted argument; CMP0168 NEW preserves this variable's scope.
FetchContent_Declare(nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    TLS_VERIFY TRUE
    HTTP_HEADER [=[User-Agent: ${ORCLI_DOWNLOAD_USER_AGENT}]=]
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    EXCLUDE_FROM_ALL SYSTEM)

set(CLI11_PRECOMPILED OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(cli11
    URL https://github.com/CLIUtils/CLI11/archive/refs/tags/v2.7.2.tar.gz
    TLS_VERIFY TRUE
    HTTP_HEADER [=[User-Agent: ${ORCLI_DOWNLOAD_USER_AGENT}]=]
    URL_HASH SHA256=46eef3101da70852ec7af026e09d485ccee81813331c8c6052d39344443b83da
    EXCLUDE_FROM_ALL SYSTEM)

FetchContent_Declare(curl
    URL https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz
    TLS_VERIFY TRUE
    HTTP_HEADER [=[User-Agent: ${ORCLI_DOWNLOAD_USER_AGENT}]=]
    URL_HASH SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
    EXCLUDE_FROM_ALL SYSTEM)

# Keep dependency tests disabled without changing this project's cached BUILD_TESTING.
set(_orcli_build_testing ${BUILD_TESTING})
set(BUILD_TESTING OFF)
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(nlohmann_json cli11)

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
set(ENABLE_THREADED_RESOLVER ON CACHE BOOL "Use asynchronous threaded DNS" FORCE)
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
    src/display.cpp
    src/fsutil.cpp
    src/http.cpp
    src/models.cpp
    src/progress.cpp
    src/request.cpp
    src/session.cpp
    src/sse.cpp)
target_include_directories(orcli_core PUBLIC src)
# OpenSSL::Crypto is linked directly because auth.cpp (RAND_bytes, EVP SHA-256) and
# credentials.cpp (OPENSSL_cleanse) call it, not only libcurl.
target_link_libraries(orcli_core PUBLIC
    nlohmann_json::nlohmann_json CURL::libcurl OpenSSL::Crypto orcli_flags)
# The help formatter is its own library so the unit tests can render pages without main.cpp.
add_library(orcli_help STATIC src/help.cpp)
target_include_directories(orcli_help PUBLIC src)
target_link_libraries(orcli_help PUBLIC CLI11::CLI11 orcli_flags)

add_executable(openrouter-cli src/main.cpp)
target_link_libraries(openrouter-cli PRIVATE orcli_core orcli_help CLI11::CLI11 orcli_flags)
target_link_options(openrouter-cli PRIVATE -static-libstdc++ -static-libgcc)

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ORCLI_IPO_SUPPORTED OUTPUT ORCLI_IPO_ERROR LANGUAGES CXX)
    if(ORCLI_IPO_SUPPORTED)
        set_property(TARGET orcli_core orcli_help openrouter-cli
            PROPERTY INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(STATUS "Link-time optimization unavailable; building without it: ${ORCLI_IPO_ERROR}")
    endif()
endif()

if(BUILD_TESTING)
    # Header-only test dependencies: SOURCE_SUBDIR names a missing directory so their own CMake
    # projects (tests, examples, optional TLS and compression probes) are never configured.
    FetchContent_Declare(doctest
        URL https://github.com/doctest/doctest/archive/refs/tags/v2.5.3.tar.gz
        TLS_VERIFY TRUE
        HTTP_HEADER [=[User-Agent: ${ORCLI_DOWNLOAD_USER_AGENT}]=]
        URL_HASH SHA256=174ebc4e769928959614789c5b4e9c3d0a0f81a62bb608756b127bfebfb21331
        SOURCE_SUBDIR no-cmake)
    FetchContent_Declare(httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.63.0.tar.gz
        TLS_VERIFY TRUE
        HTTP_HEADER [=[User-Agent: ${ORCLI_DOWNLOAD_USER_AGENT}]=]
        URL_HASH SHA256=44ab42e16926e95f7641d286bc90d739ab1c16886114d7cc5bc7a9f905c5edf8
        SOURCE_SUBDIR no-cmake)
    FetchContent_MakeAvailable(doctest httplib)
    add_subdirectory(tests)
endif()

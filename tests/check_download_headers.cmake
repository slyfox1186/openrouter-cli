cmake_minimum_required(VERSION 4.0)

if(ORCLI_HEADER_PROBE)
    # Stop at the real generated download call; no network or fake archive is needed.
    function(file)
        if(ARGV0 STREQUAL "DOWNLOAD")
            set(expected "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:159.0) Gecko/20100101 Firefox/159.0")
            set(header_count 0)
            math(EXPR last "${ARGC} - 1")
            foreach(index RANGE 0 ${last})
                if("${ARGV${index}}" STREQUAL "HTTPHEADER")
                    math(EXPR value_index "${index} + 1")
                    if(value_index GREATER_EQUAL ARGC OR NOT "${ARGV${value_index}}" STREQUAL "${expected}")
                        message(FATAL_ERROR "ORCLI_HEADER_ASSERTION_FAILED: incomplete or unexpected header")
                    endif()
                    math(EXPR header_count "${header_count} + 1")
                endif()
            endforeach()
            if(NOT header_count EQUAL 1 OR NOT CMAKE_TLS_VERIFY)
                message(FATAL_ERROR "ORCLI_HEADER_ASSERTION_FAILED: header count or TLS verification")
            endif()
            message(FATAL_ERROR "ORCLI_HEADER_VERIFIED")
        endif()

        # These operations have no caller result variable. Fail closed if CMake's path changes.
        if(NOT ARGV0 STREQUAL "MAKE_DIRECTORY" AND NOT ARGV0 STREQUAL "CONFIGURE")
            message(FATAL_ERROR "ORCLI_HEADER_UNEXPECTED_FILE: ${ARGV0}")
        endif()
        # Nonempty CONFIGURE content could expand variables shadowed by this wrapper.
        if(ARGV0 STREQUAL "CONFIGURE")
            if(NOT ARGC EQUAL 5 OR NOT ARGV1 STREQUAL "OUTPUT" OR
               NOT ARGV3 STREQUAL "CONTENT" OR NOT "${ARGV4}" STREQUAL "")
                message(FATAL_ERROR "ORCLI_HEADER_UNEXPECTED_FILE: nonempty or changed CONFIGURE operation")
            endif()
        endif()
        set(forward "_file(")
        math(EXPR last "${ARGC} - 1")
        foreach(index RANGE 0 ${last})
            if("${ARGV${index}}" STREQUAL "RESULT")
                message(FATAL_ERROR "ORCLI_HEADER_UNEXPECTED_FILE: result-bearing operation")
            endif()
            set(delimiter "=")
            while(TRUE)
                string(FIND "${ARGV${index}}" "]${delimiter}" closing)
                if(closing EQUAL -1)
                    break()
                endif()
                string(APPEND delimiter "=")
            endwhile()
            string(APPEND forward " [${delimiter}[\n${ARGV${index}}]${delimiter}]")
        endforeach()
        cmake_language(EVAL CODE "${forward})")
    endfunction()
    return()
endif()

if(NOT IS_ABSOLUTE "${ORCLI_SOURCE_DIR}")
    message(FATAL_ERROR "Pass an absolute ORCLI_SOURCE_DIR")
endif()
if(NOT ORCLI_TEST_GENERATOR)
    set(ORCLI_TEST_GENERATOR Ninja)
endif()
if(NOT ORCLI_TEST_MAKE_PROGRAM)
    find_program(ORCLI_TEST_MAKE_PROGRAM ninja REQUIRED)
endif()

file(READ "${ORCLI_SOURCE_DIR}/CMakeLists.txt" production)
string(REGEX MATCH [[cmake_minimum_required\(VERSION [^)]*\)]] minimum "${production}")
string(REGEX MATCH [[set\(ORCLI_DOWNLOAD_USER_AGENT "[^"]*"\)]] user_agent "${production}")
string(REGEX MATCHALL [[FetchContent_Declare\([^)]*\)]] declarations "${production}")
string(REGEX MATCHALL [[FetchContent_Declare\(]] occurrences "${production}")
list(LENGTH declarations count)
list(LENGTH occurrences occurrence_count)
if(NOT minimum OR NOT user_agent OR NOT count EQUAL 5 OR NOT occurrence_count EQUAL count)
    message(FATAL_ERROR "Cannot identify the production policy, User-Agent and all five dependency declarations")
endif()
set(names)
foreach(declaration IN LISTS declarations)
    string(REGEX MATCH [[FetchContent_Declare\(([A-Za-z0-9_]+)]] match "${declaration}")
    list(APPEND names "${CMAKE_MATCH_1}")
endforeach()
list(SORT names)
if(NOT names STREQUAL "cli11;curl;doctest;httplib;nlohmann_json")
    message(FATAL_ERROR "Unexpected dependency declarations: ${names}")
endif()

set(temp_base "$ENV{TMPDIR}")
if(NOT IS_ABSOLUTE "${temp_base}")
    set(temp_base /tmp)
endif()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(scratch "${temp_base}/orcli download's headers-${nonce}")
if(EXISTS "${scratch}" OR IS_SYMLINK "${scratch}")
    message(FATAL_ERROR "Scratch path already exists")
endif()
file(MAKE_DIRECTORY "${scratch}")

foreach(declaration IN LISTS declarations)
    string(REGEX MATCH [[FetchContent_Declare\(([A-Za-z0-9_]+)]] match "${declaration}")
    set(name "${CMAKE_MATCH_1}")
    # Even an upstream change that bypasses interception must never reach a public server.
    string(REGEX MATCHALL "URL https://[^ \t\r\n]+" urls "${declaration}")
    list(LENGTH urls url_count)
    if(NOT url_count EQUAL 1)
        file(REMOVE_RECURSE "${scratch}")
        message(FATAL_ERROR "${name}: cannot isolate the dependency download URL")
    endif()
    string(REGEX REPLACE "URL https://[^ \t\r\n]+"
        "URL https://127.0.0.1:0/archive.tar.xz" declaration "${declaration}")
    set(source "${scratch}/${name}")
    file(MAKE_DIRECTORY "${source}")
    file(WRITE "${source}/CMakeLists.txt"
        "${minimum}\nproject(header_probe LANGUAGES NONE)\n"
        "cmake_policy(GET CMP0168 population_policy)\n"
        "if(NOT population_policy STREQUAL \"NEW\")\n"
        "message(FATAL_ERROR \"ORCLI_HEADER_ASSERTION_FAILED: CMP0168 must be NEW\")\nendif()\n"
        "include(FetchContent)\nset(ORCLI_HEADER_PROBE TRUE)\n"
        "include([==[${CMAKE_CURRENT_LIST_FILE}]==])\n"
        "set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)\n"
        "${user_agent}\n${declaration}\nFetchContent_MakeAvailable(${name})\n"
        "message(FATAL_ERROR \"ORCLI_HEADER_ASSERTION_FAILED: download was bypassed\")\n")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "NO_PROXY=*" "no_proxy=*" --
            "${CMAKE_COMMAND}" -S "${source}" -B "${source}/build"
            -G "${ORCLI_TEST_GENERATOR}" "-DCMAKE_MAKE_PROGRAM=${ORCLI_TEST_MAKE_PROGRAM}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output TIMEOUT 30)
    if(result STREQUAL "0" OR NOT output MATCHES "ORCLI_HEADER_VERIFIED" OR
       output MATCHES "ORCLI_HEADER_ASSERTION_FAILED|ORCLI_HEADER_UNEXPECTED_FILE")
        file(REMOVE_RECURSE "${scratch}")
        message(FATAL_ERROR "${name}: download header regression failed (${result})\n${output}")
    endif()
    message(STATUS "${name}: exact download User-Agent and TLS verification confirmed")
endforeach()
file(REMOVE_RECURSE "${scratch}")

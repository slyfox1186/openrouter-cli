add_library(orcli_doctest INTERFACE)
target_include_directories(orcli_doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})

add_executable(unit_tests
    unit/main.cpp
    unit/test_account.cpp
    unit/test_auth.cpp
    unit/test_config.cpp
    unit/test_context.cpp
    unit/test_costlog.cpp
    unit/test_credentials.cpp
    unit/test_fsutil.cpp
    unit/test_http.cpp
    unit/test_help.cpp
    unit/test_models.cpp
    unit/test_progress.cpp
    unit/test_request.cpp
    unit/test_secrets.cpp
    unit/test_session.cpp
    unit/test_sse.cpp
    unit/test_terminal.cpp)
target_link_libraries(unit_tests PRIVATE orcli_core orcli_help orcli_doctest orcli_flags Threads::Threads)
# Stand-in for systemd-creds: exercises spawning, pipes, exit codes and timeouts, not encryption.
set(ORCLI_FAKE_CREDS_TOOL "${CMAKE_CURRENT_SOURCE_DIR}/fake_systemd_creds.sh")
target_compile_definitions(unit_tests PRIVATE ORCLI_FAKE_CREDS_TOOL="${ORCLI_FAKE_CREDS_TOOL}")
add_test(NAME unit COMMAND unit_tests)

add_library(orcli_httplib INTERFACE)
target_include_directories(orcli_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
target_link_libraries(orcli_httplib INTERFACE Threads::Threads)

add_executable(integration_tests
    integration/main.cpp
    integration/test_cli.cpp
    mock/mock_server.cpp)
target_include_directories(integration_tests PRIVATE mock)
target_link_libraries(integration_tests PRIVATE orcli_core orcli_doctest orcli_httplib orcli_flags)
target_compile_definitions(integration_tests PRIVATE
    ORCLI_BINARY="$<TARGET_FILE:openrouter-cli>"
    ORCLI_FAKE_CREDS_TOOL="${ORCLI_FAKE_CREDS_TOOL}")
add_dependencies(integration_tests openrouter-cli)
add_test(NAME integration COMMAND integration_tests --no-colors=true)

find_program(ORCLI_BASH bash REQUIRED)
add_test(NAME installer COMMAND "${ORCLI_BASH}" "${CMAKE_CURRENT_SOURCE_DIR}/shell/test_build.sh")
add_test(NAME download_headers COMMAND "${CMAKE_COMMAND}"
    "-DORCLI_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
    "-DORCLI_TEST_GENERATOR=${CMAKE_GENERATOR}"
    "-DORCLI_TEST_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/check_download_headers.cmake")

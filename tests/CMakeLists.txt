add_library(orcli_doctest INTERFACE)
target_include_directories(orcli_doctest SYSTEM INTERFACE ${doctest_SOURCE_DIR})

add_executable(unit_tests
    unit/main.cpp
    unit/test_config.cpp
    unit/test_context.cpp
    unit/test_costlog.cpp
    unit/test_fsutil.cpp
    unit/test_models.cpp
    unit/test_request.cpp
    unit/test_secrets.cpp
    unit/test_session.cpp
    unit/test_sse.cpp)
target_link_libraries(unit_tests PRIVATE orcli_core orcli_doctest orcli_flags)
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
    ORCLI_BINARY="$<TARGET_FILE:openrouter-cli>")
add_dependencies(integration_tests openrouter-cli)
add_test(NAME integration COMMAND integration_tests)

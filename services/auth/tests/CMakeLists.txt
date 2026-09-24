add_executable(session-verdict-test unit/session-verdict-test.cc)
target_compile_definitions(session-verdict-test PRIVATE
    ARGUS_AUTH_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(session-verdict-test PRIVATE
    argus::auth-session
    argus::auth-device
    argus::auth-rpc
    argus::lib::auth
    argus::lib::config
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::text
    argus::contracts::sync
    argus::clients::auth
    argus::clients::identity
    Drogon::Drogon
    doctest::doctest)
target_compile_options(session-verdict-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(session-verdict-test)
add_test(NAME session-verdict-test COMMAND session-verdict-test)

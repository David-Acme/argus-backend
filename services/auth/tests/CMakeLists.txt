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

add_executable(device-login-test unit/device-login-test.cc)
target_compile_definitions(device-login-test PRIVATE
    ARGUS_AUTH_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(device-login-test PRIVATE
    argus::auth-auth
    argus::auth-session
    argus::auth-device
    argus::auth-rpc
    argus::lib::auth
    argus::lib::config
    argus::lib::sqlite
    argus::contracts::auth
    argus::clients::identity
    Drogon::Drogon
    doctest::doctest)
target_compile_options(device-login-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(device-login-test)
add_test(NAME device-login-test COMMAND device-login-test)

add_executable(change-outbox-test unit/change-outbox-test.cc)
target_compile_definitions(change-outbox-test PRIVATE
    ARGUS_AUTH_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(change-outbox-test PRIVATE
    argus::auth-session
    argus::lib::sqlite
    argus::lib::text
    Drogon::Drogon
    doctest::doctest)
target_compile_options(change-outbox-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(change-outbox-test)
add_test(NAME change-outbox-test COMMAND change-outbox-test)

add_executable(auth-rate-gate-test unit/auth-rate-gate-test.cc)
target_link_libraries(auth-rate-gate-test PRIVATE
    argus::auth-auth
    argus::auth-config
    argus::lib::config
    argus::lib::errors
    argus::lib::http
    argus::contracts::auth
    Drogon::Drogon
    doctest::doctest)
target_compile_options(auth-rate-gate-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(auth-rate-gate-test)
add_test(NAME auth-rate-gate-test COMMAND auth-rate-gate-test)

add_executable(auth-migration-test unit/auth-migration-test.cc)
target_link_libraries(auth-migration-test PRIVATE
    auth-migration
    doctest::doctest)
target_compile_options(auth-migration-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(auth-migration-test)
add_test(NAME auth-migration-test COMMAND auth-migration-test)

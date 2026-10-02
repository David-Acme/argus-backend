add_executable(identity-config-test unit/identity-config-test.cc)
target_link_libraries(identity-config-test PRIVATE
    argus::identity-config
    argus::lib::http
    doctest::doctest)
target_compile_options(identity-config-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-config-test)
add_test(NAME identity-config-test COMMAND identity-config-test)

add_executable(identity-migration-test unit/identity-migration-test.cc)
target_link_libraries(identity-migration-test PRIVATE
    identity-migration
    doctest::doctest)
target_compile_options(identity-migration-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-migration-test)
add_test(NAME identity-migration-test COMMAND identity-migration-test)

add_executable(identity-change-outbox-test unit/identity-change-outbox-test.cc)
target_compile_definitions(identity-change-outbox-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-change-outbox-test PRIVATE
    argus::identity-change-outbox
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-change-outbox-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-change-outbox-test)
add_test(NAME identity-change-outbox-test COMMAND identity-change-outbox-test)

add_executable(identity-change-outbox-sink-test
    unit/identity-change-outbox-sink-test.cc)
target_compile_definitions(identity-change-outbox-sink-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-change-outbox-sink-test PRIVATE
    argus::identity-user
    argus::identity-change-outbox
    argus::lib::errors
    argus::lib::nats
    argus::lib::sqlite
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-change-outbox-sink-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-change-outbox-sink-test)
add_test(NAME identity-change-outbox-sink-test
         COMMAND identity-change-outbox-sink-test)

add_executable(identity-sync-rpc-test unit/identity-sync-rpc-test.cc)
target_compile_definitions(identity-sync-rpc-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-sync-rpc-test PRIVATE
    argus::identity-rpc
    argus::clients::identity
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-sync-rpc-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-sync-rpc-test)
add_test(NAME identity-sync-rpc-test COMMAND identity-sync-rpc-test)

add_executable(identity-change-transaction-test
    unit/identity-change-transaction-test.cc)
target_compile_definitions(identity-change-transaction-test PRIVATE
    ARGUS_IDENTITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(identity-change-transaction-test PRIVATE
    argus::identity-invitation
    argus::identity-user
    argus::identity-change-outbox
    argus::lib::nats
    argus::lib::sqlite
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(identity-change-transaction-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-change-transaction-test)
add_test(NAME identity-change-transaction-test
         COMMAND identity-change-transaction-test)

add_executable(identity-face-slots-test unit/identity-face-slots-test.cc)
target_link_libraries(identity-face-slots-test PRIVATE
    argus::identity-shared
    doctest::doctest)
target_compile_options(identity-face-slots-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(identity-face-slots-test)
add_test(NAME identity-face-slots-test COMMAND identity-face-slots-test)

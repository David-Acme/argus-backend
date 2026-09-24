add_executable(sync-surface-test unit/sync-surface-test.cc)
target_link_libraries(sync-surface-test PRIVATE
    argus::sync-transport
    argus::sync-fanout
    argus::sync-repositories
    argus::lib::config
    argus::lib::sqlite
    argus::lib::text
    argus::contracts::auth
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(sync-surface-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(sync-surface-test)
add_test(NAME sync-surface-test COMMAND sync-surface-test)

add_executable(audit-sync-read-test unit/audit-sync-read-test.cc)
target_link_libraries(audit-sync-read-test PRIVATE
    argus::sync-fanout
    argus::sync-transport
    argus::sync-repositories
    argus::lib::sqlite
    argus::lib::text
    argus::lib::errors
    argus::contracts::auth
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(audit-sync-read-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(audit-sync-read-test)
add_test(NAME audit-sync-read-test COMMAND audit-sync-read-test)

add_executable(audit-compaction-test unit/audit-compaction-test.cc)
target_compile_definitions(audit-compaction-test PRIVATE
    ARGUS_SYNC_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(audit-compaction-test PRIVATE
    argus::sync-fanout
    argus::sync-repositories
    argus::lib::sqlite
    argus::lib::text
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(audit-compaction-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(audit-compaction-test)
add_test(NAME audit-compaction-test COMMAND audit-compaction-test)

add_executable(notification-delivery-inbox-test
               unit/notification-delivery-inbox-test.cc)
target_compile_definitions(notification-delivery-inbox-test PRIVATE
    ARGUS_SYNC_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(notification-delivery-inbox-test PRIVATE
    argus::sync-fanout
    argus::lib::sqlite
    argus::lib::nats
    Drogon::Drogon
    doctest::doctest)
target_compile_options(notification-delivery-inbox-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(notification-delivery-inbox-test)
add_test(NAME notification-delivery-inbox-test
         COMMAND notification-delivery-inbox-test)

add_executable(change-feed-consumer-test unit/change-feed-consumer-test.cc)
target_compile_definitions(change-feed-consumer-test PRIVATE
    ARGUS_SYNC_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(change-feed-consumer-test PRIVATE
    argus::sync-fanout
    argus::lib::sqlite
    argus::lib::nats
    argus::lib::text
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(change-feed-consumer-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(change-feed-consumer-test)
add_test(NAME change-feed-consumer-test COMMAND change-feed-consumer-test)

add_executable(voice-leg-test unit/voice-leg-test.cc)
target_link_libraries(voice-leg-test PRIVATE
    argus::sync-transport
    argus::lib::config
    argus::lib::text
    argus::clients::voice
    doctest::doctest)
target_compile_options(voice-leg-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(voice-leg-test)
add_test(NAME voice-leg-test COMMAND voice-leg-test)

add_executable(event-sync-empty-test unit/event-sync-empty-test.cc)
target_link_libraries(event-sync-empty-test PRIVATE
    argus::sync-transport
    argus::sync-repositories
    argus::lib::sqlite
    argus::contracts::sync
    Drogon::Drogon
    sqlite-vec
    doctest::doctest)
target_compile_options(event-sync-empty-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(event-sync-empty-test)
add_test(NAME event-sync-empty-test COMMAND event-sync-empty-test)

add_executable(notification-delivery-live-test
               unit/notification-delivery-live-test.cc)
target_compile_definitions(notification-delivery-live-test PRIVATE
    ARGUS_SYNC_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(notification-delivery-live-test PRIVATE
    argus::sync-fanout
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::text
    argus::contracts::notification
    Drogon::Drogon
    doctest::doctest)
target_compile_options(notification-delivery-live-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(notification-delivery-live-test)
add_test(NAME notification-delivery-live-test
         COMMAND notification-delivery-live-test)

add_executable(change-feed-live-test unit/change-feed-live-test.cc)
target_compile_definitions(change-feed-live-test PRIVATE
    ARGUS_SYNC_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(change-feed-live-test PRIVATE
    argus::sync-fanout
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::text
    argus::contracts::sync
    Drogon::Drogon
    doctest::doctest)
target_compile_options(change-feed-live-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(change-feed-live-test)
add_test(NAME change-feed-live-test COMMAND change-feed-live-test)

add_executable(sync-migration-test unit/sync-migration-test.cc)
target_link_libraries(sync-migration-test PRIVATE
    sync-migration
    doctest::doctest)
target_compile_options(sync-migration-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(sync-migration-test)
add_test(NAME sync-migration-test COMMAND sync-migration-test)

add_executable(golden-sync-test e2e/golden-sync-test.cc)
target_compile_definitions(golden-sync-test PRIVATE
    ARGUS_TEST_SYNC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/sync")
target_link_libraries(golden-sync-test PRIVATE Drogon::Drogon)
target_compile_options(golden-sync-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(golden-sync-test)
add_test(NAME golden-sync-test COMMAND golden-sync-test)

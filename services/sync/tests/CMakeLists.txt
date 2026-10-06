add_executable(sync-surface-test unit/sync-surface-test.cc)
target_link_libraries(sync-surface-test PRIVATE
    argus::sync-transport
    argus::sync-fanout
    argus::sync-repositories
    argus::lib::auth
    argus::lib::config
    argus::lib::nats
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

add_executable(rtc-token-test unit/rtc-token-test.cc)
target_link_libraries(rtc-token-test PRIVATE
    argus::sync-rtc
    argus::lib::errors
    Drogon::Drogon
    doctest::doctest)
target_compile_options(rtc-token-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(rtc-token-test)
add_test(NAME rtc-token-test COMMAND rtc-token-test)

add_executable(heartbeat-test unit/heartbeat-test.cc)
target_link_libraries(heartbeat-test PRIVATE
    argus::sync-heartbeat
    argus::lib::text
    argus::lib::nats
    Drogon::Drogon
    doctest::doctest)
target_compile_options(heartbeat-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(heartbeat-test)
add_test(NAME heartbeat-test COMMAND heartbeat-test)

add_executable(sync-control-callers-test unit/sync-control-callers-test.cc)
target_link_libraries(sync-control-callers-test PRIVATE
    argus::sync-control-rpc
    doctest::doctest)
target_compile_options(sync-control-callers-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(sync-control-callers-test)
add_test(NAME sync-control-callers-test COMMAND sync-control-callers-test)

add_executable(sync-module-data-test unit/sync-module-data-test.cc)
target_compile_definitions(sync-module-data-test PRIVATE
    ARGUS_SYNC_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../database/schema.sql")
target_link_libraries(sync-module-data-test PRIVATE
    argus::sync-module-data
    argus::lib::sqlite
    Drogon::Drogon
    doctest::doctest)
target_compile_options(sync-module-data-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(sync-module-data-test)
add_test(NAME sync-module-data-test COMMAND sync-module-data-test)

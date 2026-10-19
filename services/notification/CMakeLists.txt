cmake_minimum_required(VERSION 3.25)
project(argus-notification VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::test-support)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/test-support
                     ${CMAKE_BINARY_DIR}/test-support)
endif()

set(NOTIFICATION_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)

    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()

    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()

    if(NOT TARGET argus::contracts::notification)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/notification
                         ${CMAKE_BINARY_DIR}/contracts/notification)
    endif()

    if(NOT TARGET argus::contracts::push)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/push
                         ${CMAKE_BINARY_DIR}/contracts/push)
    endif()

    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::lib::text)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/text
                         ${CMAKE_BINARY_DIR}/text)
    endif()
    if(NOT TARGET argus::lib::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::lib::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::lib::outbox)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/outbox
                         ${CMAKE_BINARY_DIR}/outbox)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
endif()

find_package(doctest REQUIRED)

if(NOT TARGET argus::contracts::camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/camera
                     ${CMAKE_BINARY_DIR}/contracts/camera)
endif()

if(NOT TARGET argus::contracts::llm)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/llm
                     ${CMAKE_BINARY_DIR}/contracts/llm)
endif()

if(NOT TARGET argus::clients::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::clients::voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/voice
                     ${CMAKE_BINARY_DIR}/clients/voice EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::clients::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/sync
                     ${CMAKE_BINARY_DIR}/clients/sync EXCLUDE_FROM_ALL)
endif()

if(NOT COMMAND argus_settings_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/settings
                     ${CMAKE_BINARY_DIR}/contracts/settings)
endif()

if(NOT TARGET argus::lib::runtime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                     ${CMAKE_BINARY_DIR}/runtime)
endif()


add_subdirectory(src/config)
add_subdirectory(src/shared)
add_subdirectory(src/shared/services/change-sink)
add_subdirectory(src/shared/services/delivery-sink)
add_subdirectory(src/shared/services/push-delivery)

file(GLOB NOTIFICATION_FEATURE_MODULES CONFIGURE_DEPENDS
    "${NOTIFICATION_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS NOTIFICATION_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/app/rpc)

add_executable(argus-notification
    ${NOTIFICATION_SRC_ROOT}/app/main.cc)

target_link_libraries(argus-notification PRIVATE
    argus::notification-config
    argus::notification-change-sink
    argus::notification-delivery-sink
    argus::notification-shared
    $<LINK_LIBRARY:WHOLE_ARCHIVE,argus::notification-feature>
    argus::notification-rpc
    argus::notification-camera-notification
    argus::notification-module-request
    argus::notification-call
    argus::notification-conversation
    argus::notification-settings
    argus::contracts::settings-wire
    argus::lib::auth argus::lib::validation argus::lib::http
    argus::clients::notification)

target_compile_options(argus-notification PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT TARGET notification-migration)
    add_subdirectory(tools/migrate-notification)
endif()

find_package(doctest REQUIRED)

foreach(TEST_NAME IN ITEMS notification-schema-test notification-migration-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
        target_link_libraries(${TEST_NAME} PRIVATE
    argus::lib::auth
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::validation
    argus::lib::http
    notification-migration
    argus::lib::doctest-main)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

add_executable(notification-controller-test
    tests/unit/notification-controller-test.cc)

set_target_properties(notification-controller-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-controller-test PRIVATE
    argus::notification-shared
    argus::notification-feature
    argus::lib::doctest-main)

target_compile_options(notification-controller-test PRIVATE -Wall -Wextra)

add_test(NAME notification-controller-test
         COMMAND notification-controller-test)

add_executable(notification-rpc-test
    tests/unit/notification-rpc-test.cc)
target_link_libraries(notification-rpc-test PRIVATE argus::lib::test-support)

set_target_properties(notification-rpc-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-rpc-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-rpc-test PRIVATE
    argus::notification-shared
    argus::notification-rpc
    argus::lib::doctest-main)

target_compile_options(notification-rpc-test PRIVATE -Wall -Wextra)

add_test(NAME notification-rpc-test COMMAND notification-rpc-test)

add_executable(notification-conversation-test
    tests/unit/notification-conversation-test.cc)
set_target_properties(notification-conversation-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(notification-conversation-test PRIVATE
    argus::notification-conversation
    argus::notification-rpc
    argus::lib::doctest-main)
target_compile_options(notification-conversation-test PRIVATE -Wall -Wextra)
add_test(NAME notification-conversation-test COMMAND notification-conversation-test)

add_executable(notification-no-nats-test
    tests/unit/notification-no-nats-test.cc)
target_link_libraries(notification-no-nats-test PRIVATE argus::lib::test-support)

set_target_properties(notification-no-nats-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-no-nats-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-no-nats-test PRIVATE
    argus::notification-shared
    argus::notification-rpc
    argus::lib::doctest-main)

target_compile_options(notification-no-nats-test PRIVATE -Wall -Wextra)

add_test(NAME notification-no-nats-test COMMAND notification-no-nats-test)

add_executable(notification-delivery-test
    tests/unit/notification-delivery-test.cc)

set_target_properties(notification-delivery-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-delivery-test PRIVATE
    argus::notification-shared
    argus::lib::sqlite
    argus::lib::doctest-main)

target_compile_options(notification-delivery-test PRIVATE -Wall -Wextra)

add_test(NAME notification-delivery-test COMMAND notification-delivery-test)

add_executable(notification-change-outbox-sink-test
    tests/unit/notification-change-outbox-sink-test.cc)

set_target_properties(notification-change-outbox-sink-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-change-outbox-sink-test PRIVATE
    argus::notification-change-sink
    argus::lib::nats
    argus::lib::doctest-main)

target_compile_definitions(notification-change-outbox-sink-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(notification-change-outbox-sink-test
                       PRIVATE -Wall -Wextra)

argus_nats_scratch_test(NAME notification-change-outbox-sink-test)

add_executable(notification-change-transaction-test
    tests/unit/notification-change-transaction-test.cc)

set_target_properties(notification-change-transaction-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-change-transaction-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-change-transaction-test PRIVATE
    argus::notification-change-sink
    argus::notification-shared
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::doctest-main)

target_compile_options(notification-change-transaction-test
                       PRIVATE -Wall -Wextra)

add_test(NAME notification-change-transaction-test
         COMMAND notification-change-transaction-test)

add_executable(notification-ack-test tests/unit/notification-ack-test.cc)
target_link_libraries(notification-ack-test PRIVATE argus::lib::test-support)

set_target_properties(notification-ack-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-ack-test PRIVATE
    argus::notification-shared
    argus::lib::sqlite
    argus::lib::doctest-main)

target_compile_options(notification-ack-test PRIVATE -Wall -Wextra)

add_test(NAME notification-ack-test COMMAND notification-ack-test)

add_executable(notification-delivery-live-test
    tests/unit/notification-delivery-live-test.cc)

set_target_properties(notification-delivery-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(notification-delivery-live-test PRIVATE
    ${NOTIFICATION_SRC_ROOT})

target_compile_definitions(notification-delivery-live-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-delivery-live-test PRIVATE
    argus::notification-delivery-sink
    argus::notification-shared
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::doctest-main)

target_compile_options(notification-delivery-live-test PRIVATE -Wall -Wextra)

argus_nats_scratch_test(NAME notification-delivery-live-test)

add_executable(call-push-routing-test tests/unit/call-push-routing-test.cc)

set_target_properties(call-push-routing-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(call-push-routing-test PRIVATE
    argus::notification-call
    argus::contracts::push
    argus::lib::doctest-main)

target_compile_options(call-push-routing-test PRIVATE -Wall -Wextra)

add_test(NAME call-push-routing-test COMMAND call-push-routing-test)

add_executable(push-sealer-test tests/unit/push-sealer-test.cc)

set_target_properties(push-sealer-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(push-sealer-test PRIVATE
    argus::notification-call
    argus::contracts::push
    argus::lib::text
    argus::lib::doctest-main)

target_compile_definitions(push-sealer-test PRIVATE
    ARGUS_PUSH_SEALED_VECTOR_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../tunnel/tests/vectors/push-sealed.json")
target_compile_options(push-sealer-test PRIVATE -Wall -Wextra)

add_test(NAME push-sealer-test COMMAND push-sealer-test)

add_executable(call-push-delivery-test tests/unit/call-push-delivery-test.cc)

set_target_properties(call-push-delivery-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(call-push-delivery-test PRIVATE
    argus::notification-call
    argus::notification-push-delivery
    argus::contracts::push
    argus::lib::text
    argus::lib::doctest-main)

target_compile_options(call-push-delivery-test PRIVATE -Wall -Wextra)

add_executable(push-alert-intent-test tests/unit/push-alert-intent-test.cc)

set_target_properties(push-alert-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(push-alert-intent-test PRIVATE
    argus::notification-push-delivery
    argus::lib::doctest-main)

target_compile_options(push-alert-intent-test PRIVATE -Wall -Wextra)

add_test(NAME push-alert-intent-test COMMAND push-alert-intent-test)

add_test(NAME call-push-delivery-test COMMAND call-push-delivery-test)

add_executable(push-alert-writer-test tests/unit/push-alert-writer-test.cc)

set_target_properties(push-alert-writer-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(push-alert-writer-test PRIVATE
    argus::notification-push-delivery
    argus::contracts::push
    argus::lib::text
    argus::lib::doctest-main)

target_compile_options(push-alert-writer-test PRIVATE -Wall -Wextra)

add_test(NAME push-alert-writer-test COMMAND push-alert-writer-test)

add_executable(notification-boot-probe-test tests/unit/notification-boot-probe-test.cc)

set_target_properties(notification-boot-probe-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-boot-probe-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-boot-probe-test PRIVATE
    argus::lib::test-support
    argus::notification-shared
    argus::lib::sqlite
    argus::lib::doctest-main)

target_compile_options(notification-boot-probe-test PRIVATE -Wall -Wextra)

add_test(NAME notification-boot-probe-test COMMAND notification-boot-probe-test)

add_executable(notification-push-outbox-test tests/unit/notification-push-outbox-test.cc)

set_target_properties(notification-push-outbox-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-push-outbox-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-push-outbox-test PRIVATE
    argus::lib::test-support
    argus::notification-change-sink
    argus::notification-shared
    argus::notification-call
    argus::notification-push-delivery
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::doctest-main)

target_compile_options(notification-push-outbox-test PRIVATE -Wall -Wextra)

add_test(NAME notification-push-outbox-test COMMAND notification-push-outbox-test)

add_executable(register-notification-token-dto-test tests/unit/register-notification-token-dto-test.cc)

set_target_properties(register-notification-token-dto-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(register-notification-token-dto-test PRIVATE
    argus::notification-feature
    argus::lib::doctest-main)

target_compile_options(register-notification-token-dto-test PRIVATE -Wall -Wextra)

add_test(NAME register-notification-token-dto-test COMMAND register-notification-token-dto-test)

add_executable(camera-notifier-test
    tests/unit/camera-notifier-test.cc)
target_link_libraries(camera-notifier-test PRIVATE argus::lib::test-support)

set_target_properties(camera-notifier-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(camera-notifier-test PRIVATE
    ${NOTIFICATION_SRC_ROOT})

target_compile_definitions(camera-notifier-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(camera-notifier-test PRIVATE
    argus::notification-camera-notification
    argus::clients::identity
    argus::lib::config
    argus::lib::sqlite
    argus::lib::text
    argus::lib::doctest-main)

target_compile_options(camera-notifier-test PRIVATE -Wall -Wextra)

add_test(NAME camera-notifier-test COMMAND camera-notifier-test)

add_executable(notification-module-request-test
    tests/unit/notification-module-request-test.cc)
target_link_libraries(notification-module-request-test PRIVATE argus::lib::test-support)

set_target_properties(notification-module-request-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(notification-module-request-test PRIVATE
    ${NOTIFICATION_SRC_ROOT})

target_compile_definitions(notification-module-request-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-module-request-test PRIVATE
    argus::notification-module-request
    argus::clients::identity
    argus::lib::config
    argus::lib::sqlite
    argus::lib::text
    argus::lib::doctest-main)

target_compile_options(notification-module-request-test PRIVATE -Wall -Wextra)

add_test(NAME notification-module-request-test COMMAND notification-module-request-test)

add_executable(notification-settings-test
    tests/unit/notification-settings-test.cc)

set_target_properties(notification-settings-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-settings-test PRIVATE
    argus::notification-settings
    argus::notification-config
    argus::notification-rpc
    argus::notification-camera-notification
    argus::contracts::settings-wire
    argus::lib::doctest-main)

target_compile_options(notification-settings-test PRIVATE -Wall -Wextra)

add_test(NAME notification-settings-test COMMAND notification-settings-test)

foreach(TEST_NAME IN ITEMS call-policy-test call-copy-test call-engine-test call-verdict-wire-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
    target_compile_definitions(${TEST_NAME} PRIVATE
        ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
    target_link_libraries(${TEST_NAME} PRIVATE
        argus::notification-call
        argus::lib::sqlite
        argus::lib::test-support
        argus::lib::doctest-main)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(notification-rpc-server-test tests/unit/notification-rpc-server-test.cc)
set_target_properties(notification-rpc-server-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(notification-rpc-server-test PRIVATE
    argus::notification-rpc
    argus::lib::grpc-test-support
    argus::lib::doctest-main)
target_compile_options(notification-rpc-server-test PRIVATE -Wall -Wextra)
add_test(NAME notification-rpc-server-test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/internal-tls-test.sh
                 $<TARGET_FILE:notification-rpc-server-test> 4)
set_tests_properties(notification-rpc-server-test PROPERTIES SKIP_RETURN_CODE 77)

cmake_minimum_required(VERSION 3.25)
project(argus-notification VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

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
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
endif()

find_package(doctest REQUIRED)

if(NOT TARGET argus::clients::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::lib::runtime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                     ${CMAKE_BINARY_DIR}/runtime)
endif()

add_subdirectory(src/shared/repositories/change-outbox)

add_library(notification-core STATIC
    ${NOTIFICATION_SRC_ROOT}/notification/nats-notification-change-sink.cc
    ${NOTIFICATION_SRC_ROOT}/notification/nats-notification-delivery-sink.cc
    ${NOTIFICATION_SRC_ROOT}/notification/notification-config.cc
    ${NOTIFICATION_SRC_ROOT}/shared/repositories/notification/notification-repository.cc
    ${NOTIFICATION_SRC_ROOT}/shared/schemas/notification/notification-schema.cc
    ${NOTIFICATION_SRC_ROOT}/shared/services/notification/notification-service.cc
)

target_include_directories(notification-core PUBLIC
    ${NOTIFICATION_SRC_ROOT})

target_link_libraries(notification-core PUBLIC
    argus::contracts::notification
    argus::contracts::sync
    argus::lib::nats
    argus::lib::text
    argus::lib::sqlite
    argus::notification-change-outbox
    ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(notification-core PRIVATE -Wall -Wextra)

add_subdirectory(src/feature/camera-notification)

set(NOTIFICATION_FEATURE_SOURCES
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/controllers/notification-controller.cc
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/controllers/notification-token-controller.cc
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/dtos/notification-read-dto.cc
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/dtos/notification-ack-dto.cc
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/dtos/delivery-summary-dto.cc
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/dtos/register-notification-token-dto.cc
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/services/notification-feature-service.cc
    ${NOTIFICATION_SRC_ROOT}/feature/api/notification/services/notification-token-feature-service.cc
    ${NOTIFICATION_SRC_ROOT}/shared/repositories/notification-token/notification-token-repository.cc
    ${NOTIFICATION_SRC_ROOT}/shared/schemas/notification-token/notification-token-schema.cc
    ${NOTIFICATION_SRC_ROOT}/shared/services/notification-token/notification-token-service.cc
)

add_executable(argus-notification
    ${NOTIFICATION_SRC_ROOT}/main.cc
    ${NOTIFICATION_SRC_ROOT}/feature/rpc/notification-rpc-service.cc
    ${NOTIFICATION_FEATURE_SOURCES})

target_link_libraries(argus-notification PRIVATE notification-core
    argus::notification-camera-notification
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
    doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

add_executable(notification-controller-test
    tests/unit/notification-controller-test.cc
    ${NOTIFICATION_FEATURE_SOURCES})

set_target_properties(notification-controller-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(notification-controller-test PRIVATE
    ${NOTIFICATION_SRC_ROOT})

target_link_libraries(notification-controller-test PRIVATE
    notification-core
    argus::lib::auth
    argus::lib::config
    argus::lib::text
    argus::lib::sqlite
    argus::lib::validation
    argus::lib::http
    argus::contracts::auth
    argus::contracts::sync
    doctest::doctest)

target_compile_options(notification-controller-test PRIVATE -Wall -Wextra)

add_test(NAME notification-controller-test
         COMMAND notification-controller-test)

add_executable(notification-rpc-test
    tests/unit/notification-rpc-test.cc
    ${NOTIFICATION_SRC_ROOT}/feature/rpc/notification-rpc-service.cc)

set_target_properties(notification-rpc-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(notification-rpc-test PRIVATE
    ${NOTIFICATION_SRC_ROOT})

target_compile_definitions(notification-rpc-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-rpc-test PRIVATE
    notification-core
    argus::clients::notification
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-rpc-test PRIVATE -Wall -Wextra)

add_test(NAME notification-rpc-test COMMAND notification-rpc-test)

add_executable(notification-no-nats-test
    tests/unit/notification-no-nats-test.cc
    ${NOTIFICATION_SRC_ROOT}/feature/rpc/notification-rpc-service.cc)

set_target_properties(notification-no-nats-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(notification-no-nats-test PRIVATE
    ${NOTIFICATION_SRC_ROOT})

target_compile_definitions(notification-no-nats-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-no-nats-test PRIVATE
    notification-core
    argus::clients::notification
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-no-nats-test PRIVATE -Wall -Wextra)

add_test(NAME notification-no-nats-test COMMAND notification-no-nats-test)

add_executable(notification-delivery-test
    tests/unit/notification-delivery-test.cc)

set_target_properties(notification-delivery-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-delivery-test PRIVATE
    notification-core
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-delivery-test PRIVATE -Wall -Wextra)

add_test(NAME notification-delivery-test COMMAND notification-delivery-test)

add_executable(notification-change-outbox-test
    tests/unit/notification-change-outbox-test.cc)

set_target_properties(notification-change-outbox-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-change-outbox-test PRIVATE
    argus::notification-change-outbox
    doctest::doctest)

target_compile_definitions(notification-change-outbox-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(notification-change-outbox-test PRIVATE -Wall -Wextra)

add_test(NAME notification-change-outbox-test
         COMMAND notification-change-outbox-test)

add_executable(notification-change-outbox-sink-test
    tests/unit/notification-change-outbox-sink-test.cc)

set_target_properties(notification-change-outbox-sink-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-change-outbox-sink-test PRIVATE
    notification-core
    argus::lib::nats
    doctest::doctest)

target_compile_definitions(notification-change-outbox-sink-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(notification-change-outbox-sink-test
                       PRIVATE -Wall -Wextra)

add_test(NAME notification-change-outbox-sink-test
         COMMAND notification-change-outbox-sink-test)

add_executable(notification-change-transaction-test
    tests/unit/notification-change-transaction-test.cc)

set_target_properties(notification-change-transaction-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-change-transaction-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-change-transaction-test PRIVATE
    notification-core
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-change-transaction-test
                       PRIVATE -Wall -Wextra)

add_test(NAME notification-change-transaction-test
         COMMAND notification-change-transaction-test)

add_executable(notification-ack-test tests/unit/notification-ack-test.cc)

set_target_properties(notification-ack-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-ack-test PRIVATE
    notification-core
    argus::lib::sqlite
    doctest::doctest)

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
    notification-core
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-delivery-live-test PRIVATE -Wall -Wextra)

add_test(NAME notification-delivery-live-test COMMAND notification-delivery-live-test)

add_executable(push-intent-test tests/unit/push-intent-test.cc)

set_target_properties(push-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(push-intent-test PRIVATE
    notification-core
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(push-intent-test PRIVATE -Wall -Wextra)

add_test(NAME push-intent-test COMMAND push-intent-test)

add_executable(camera-notifier-test
    tests/unit/camera-notifier-test.cc)

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
    doctest::doctest)

target_compile_options(camera-notifier-test PRIVATE -Wall -Wextra)

add_test(NAME camera-notifier-test COMMAND camera-notifier-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

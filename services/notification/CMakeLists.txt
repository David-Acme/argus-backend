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

add_subdirectory(src/config)
add_subdirectory(src/shared)
add_subdirectory(src/shared/services/change-sink)
add_subdirectory(src/shared/services/delivery-sink)

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
    tests/unit/notification-controller-test.cc)

set_target_properties(notification-controller-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-controller-test PRIVATE
    argus::notification-shared
    argus::notification-feature
    doctest::doctest)

target_compile_options(notification-controller-test PRIVATE -Wall -Wextra)

add_test(NAME notification-controller-test
         COMMAND notification-controller-test)

add_executable(notification-rpc-test
    tests/unit/notification-rpc-test.cc)

set_target_properties(notification-rpc-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-rpc-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-rpc-test PRIVATE
    argus::notification-shared
    argus::notification-rpc
    doctest::doctest)

target_compile_options(notification-rpc-test PRIVATE -Wall -Wextra)

add_test(NAME notification-rpc-test COMMAND notification-rpc-test)

add_executable(notification-no-nats-test
    tests/unit/notification-no-nats-test.cc)

set_target_properties(notification-no-nats-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(notification-no-nats-test PRIVATE
    ARGUS_NOTIFICATION_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(notification-no-nats-test PRIVATE
    argus::notification-shared
    argus::notification-rpc
    doctest::doctest)

target_compile_options(notification-no-nats-test PRIVATE -Wall -Wextra)

add_test(NAME notification-no-nats-test COMMAND notification-no-nats-test)

add_executable(notification-delivery-test
    tests/unit/notification-delivery-test.cc)

set_target_properties(notification-delivery-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-delivery-test PRIVATE
    argus::notification-shared
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
    argus::notification-change-sink
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
    argus::notification-change-sink
    argus::notification-shared
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
    argus::notification-shared
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
    argus::notification-delivery-sink
    argus::notification-shared
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-delivery-live-test PRIVATE -Wall -Wextra)

add_test(NAME notification-delivery-live-test COMMAND notification-delivery-live-test)

add_executable(push-intent-test tests/unit/push-intent-test.cc)

set_target_properties(push-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(push-intent-test PRIVATE
    argus::notification-shared
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

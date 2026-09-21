cmake_minimum_required(VERSION 3.25)
project(argus-notification VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(NOTIFICATION_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    # Standalone configure: this service owns the whole build, so it adds the
    # packages it links — the leaves it uses directly and the sibling packages
    # that used to arrive through argus-common — plus the third-party
    # sqlite-vec, so the notification service links the same objects as any other
    # consumer.

    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()

    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()

    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

    # argus_identity compiles the face services, whose headers need ncnn; the
    # objects are dropped at link time because nothing in this service
    # references them (verified with nm -C: zero AI symbols).
    set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NCNN_OPENMP ON CACHE BOOL "" FORCE)
    set(NCNN_AVX2 ON CACHE BOOL "" FORCE)
    set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
    set(NCNN_ENABLE_PIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ncnn
                     ${CMAKE_BINARY_DIR}/third_party/ncnn EXCLUDE_FROM_ALL)
    set(ARGUS_NCNN_TARGET ncnn)

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
    if(NOT TARGET argus::audit)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/audit
                         ${CMAKE_BINARY_DIR}/audit)
    endif()
    if(NOT TARGET argus::lib::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/cert
                         ${CMAKE_BINARY_DIR}/cert)
    endif()
    if(NOT TARGET argus::room)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/room
                         ${CMAKE_BINARY_DIR}/room)
    endif()
    if(NOT TARGET argus::socket)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/socket
                         ${CMAKE_BINARY_DIR}/socket)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity
                         ${CMAKE_BINARY_DIR}/identity)
    endif()
    if(NOT TARGET identity-migration)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity/tools/migrate-identity
                         ${CMAKE_BINARY_DIR}/identity/tools/migrate-identity)
    endif()
    if(NOT TARGET argus_sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/sync
                         ${CMAKE_BINARY_DIR}/sync)
    endif()
endif()

find_package(doctest REQUIRED)

# The notification RPC contract (argus.notification.v1.NotificationService);
# this service is the owner and serves it, consumers link
# argus::clients::notification.
if(NOT TARGET argus::clients::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()

add_library(notification-core STATIC
    ${NOTIFICATION_SRC_ROOT}/notification/nats-notification-change-sink.cc
    ${NOTIFICATION_SRC_ROOT}/notification/nats-notification-delivery-sink.cc
    ${NOTIFICATION_SRC_ROOT}/notification/notification-config.cc
)

target_include_directories(notification-core PUBLIC
    ${NOTIFICATION_SRC_ROOT})

target_link_libraries(notification-core PUBLIC argus_sync argus::lib::sqlite
    ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(notification-core PRIVATE -Wall -Wextra)

# The write-side feature surface (markAsRead + registerToken controllers,
# feature services, DTOs) compiles from the shared tree into THIS executable
# only: the controllers are AutoCreation and register their routes during
# static init, exactly like the legacy binary does. The legacy keeps its own
# registration until F3-2; the notification repository and schema ride
# argus_sync, the notification-token repository/service are not in argus_sync
# and compile from the shared tree too.
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

# The filters validate through argus.identity.v1 (argus::clients::identity rides
# argus::lib::auth); the RPC owner serves argus.notification.v1 via
# argus::clients::notification.
target_link_libraries(argus-notification PRIVATE notification-core
    argus::lib::auth argus::clients::notification)

target_compile_options(argus-notification PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# notification-migration builds only as a dependency of the migration suite.
# The standalone build adds its folder here; the root tree finds it via the
# discovery glob.
if(PROJECT_IS_TOP_LEVEL AND NOT TARGET notification-migration)
    add_subdirectory(tools/migrate-notification)
endif()

# The unit suites opt back into the default build: this folder is added
# EXCLUDE_FROM_ALL, and notification-migration (tools/) stays out of it.
find_package(doctest REQUIRED)

foreach(TEST_NAME IN ITEMS notification-schema-test notification-migration-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
        target_link_libraries(${TEST_NAME} PRIVATE
    argus::audit
    argus::lib::auth
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::socket
    argus::lib::sqlite
    argus::lib::validation
    argus::lib::http
    notification-migration
    doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# The controller suite compiles the same write-side sources the service
# compiles and rides the sync layer for repositories and boot plumbing.
add_executable(notification-controller-test
    tests/unit/notification-controller-test.cc
    ${NOTIFICATION_FEATURE_SOURCES})

set_target_properties(notification-controller-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(notification-controller-test PRIVATE
    ${NOTIFICATION_SRC_ROOT})

target_link_libraries(notification-controller-test PRIVATE
    argus_sync
    argus::lib::auth
    argus::lib::config
    argus::lib::text
    argus::socket
    argus::lib::sqlite
    argus::lib::validation
    argus::lib::http
    argus::contracts::auth
    argus::contracts::sync
    doctest::doctest)

target_compile_options(notification-controller-test PRIVATE -Wall -Wextra)

add_test(NAME notification-controller-test
         COMMAND notification-controller-test)

# The RPC suite boots the real schema and the real service against it.
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
    argus_sync
    argus::clients::notification
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-rpc-test PRIVATE -Wall -Wextra)

add_test(NAME notification-rpc-test COMMAND notification-rpc-test)

# No-NATS survival: with no sink installed the service stays alive, names
# the outcome and leaves intents pending instead of terminating.
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
    argus_sync
    argus::clients::notification
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-no-nats-test PRIVATE -Wall -Wextra)

add_test(NAME notification-no-nats-test COMMAND notification-no-nats-test)

# Durable delivery seam: intents settle only after the broker stores them.
add_executable(notification-delivery-test
    tests/unit/notification-delivery-test.cc)

set_target_properties(notification-delivery-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-delivery-test PRIVATE
    argus_sync
    argus::socket
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-delivery-test PRIVATE -Wall -Wextra)

add_test(NAME notification-delivery-test COMMAND notification-delivery-test)

add_executable(notification-ack-test tests/unit/notification-ack-test.cc)

set_target_properties(notification-ack-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(notification-ack-test PRIVATE
    argus_sync
    argus::socket
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-ack-test PRIVATE -Wall -Wextra)

add_test(NAME notification-ack-test COMMAND notification-ack-test)

# Opt-in live check against a real NATS; skips without ARGUS_NATS_URL.
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
    argus_sync
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(notification-delivery-live-test PRIVATE -Wall -Wextra)

add_test(NAME notification-delivery-live-test COMMAND notification-delivery-live-test)

# The push-intent publish seam rides the owner: the camera-notifier create
# enters here, so the sink hook is tested here.
add_executable(push-intent-test tests/unit/push-intent-test.cc)

set_target_properties(push-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(push-intent-test PRIVATE
    argus_sync
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(push-intent-test PRIVATE -Wall -Wextra)

add_test(NAME push-intent-test COMMAND push-intent-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

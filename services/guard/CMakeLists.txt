cmake_minimum_required(VERSION 3.25)
project(argus-guard VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(GUARD_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    # Standalone configure: this service owns the whole build, so it adds the
    # packages it links — the leaves it uses directly and the sibling packages
    # that used to arrive through argus-common — plus the third-party
    # sqlite-vec, so the guard service links the same objects as any other
    # consumer.
    if(NOT TARGET argus_client_grpc_bridge_entry)
        add_library(argus_client_grpc_bridge_entry INTERFACE)
        add_library(argus_client_grpc_bridge_exit INTERFACE)
    endif()
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

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

    if(NOT TARGET argus::access)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/access
                         ${CMAKE_BINARY_DIR}/access)
    endif()
    if(NOT TARGET argus::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::hash)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/hash
                         ${CMAKE_BINARY_DIR}/hash)
    endif()
    if(NOT TARGET argus::json)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/json
                         ${CMAKE_BINARY_DIR}/json)
    endif()
    if(NOT TARGET argus::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET pkg::response)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/response
                         ${CMAKE_BINARY_DIR}/response)
    endif()
    if(NOT TARGET argus::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::client-identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                         ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::client-notification)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                         ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::client-camera-actions)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera-actions
                         ${CMAKE_BINARY_DIR}/clients/camera-actions EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::audit)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/audit
                         ${CMAKE_BINARY_DIR}/audit)
    endif()
    if(NOT TARGET argus::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/cert
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
    if(NOT TARGET argus::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/auth
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
    if(NOT TARGET argus::llm-client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm-client
                         ${CMAKE_BINARY_DIR}/llm-client)
    endif()
    if(NOT TARGET argus::vlm-client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/vlm-client
                         ${CMAKE_BINARY_DIR}/vlm-client)
    endif()
endif()

find_package(doctest REQUIRED)

add_subdirectory(src/feature/guard)
add_subdirectory(src/feature/api/guard)

add_executable(argus-guard ${GUARD_SRC_ROOT}/main.cc)
target_include_directories(argus-guard PRIVATE ${GUARD_SRC_ROOT})
target_link_libraries(argus-guard PRIVATE
    argus::guard
    argus::guard-api
    argus::auth
    argus::config
    argus::nats
    argus::sqlite
    argus::client-identity
    argus::client-notification
    argus::client-camera-actions
    argus::llm-client
    argus::vlm-client
    ${ARGUS_SQLITE_VEC_TARGET})
target_compile_options(argus-guard PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

foreach(TEST_NAME IN ITEMS guard-policy-test guard-assessment-test
        guard-risk-test guard-schema-test guard-migration-test
        guard-adversarial-test guard-dialogue-test guard-saga-test
        guard-dialogue-service-test guard-dlq-crash-test
        guard-retry-lifecycle-test guard-belief-test
        guard-decision-journal-test guard-notify-thread-test
        guard-belief-scope-test guard-outbox-adopt-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)

    # This folder is added EXCLUDE_FROM_ALL, so the suites have to opt back
    # into the default build or ctest registers tests whose binaries never
    # get built.
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)

    target_link_libraries(${TEST_NAME} PRIVATE argus::guard doctest::doctest)
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    if(TEST_NAME STREQUAL "guard-decision-journal-test"
       OR TEST_NAME STREQUAL "guard-notify-thread-test")
        target_link_libraries(${TEST_NAME} PRIVATE argus::guard-api)
    endif()
    if(TEST_NAME STREQUAL "guard-schema-test"
       OR TEST_NAME STREQUAL "guard-migration-test"
       OR TEST_NAME STREQUAL "guard-saga-test"
       OR TEST_NAME STREQUAL "guard-retry-lifecycle-test"
       OR TEST_NAME STREQUAL "guard-dialogue-service-test"
       OR TEST_NAME STREQUAL "guard-decision-journal-test"
       OR TEST_NAME STREQUAL "guard-notify-thread-test"
       OR TEST_NAME STREQUAL "guard-belief-scope-test"
       OR TEST_NAME STREQUAL "guard-outbox-adopt-test"
       OR TEST_NAME STREQUAL "guard-dlq-crash-test")
        target_compile_definitions(${TEST_NAME} PRIVATE
            ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
    endif()
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# Opt-in live check driving the real GuardService against a real NATS.
add_executable(guard-dlq-service-live-test
    tests/unit/guard-dlq-service-live-test.cc)
set_target_properties(guard-dlq-service-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-dlq-service-live-test PRIVATE
    argus::guard
    argus::json
    argus::nats
    argus::sqlite
    doctest::doctest)
target_include_directories(guard-dlq-service-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_definitions(guard-dlq-service-live-test PRIVATE
    ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(guard-dlq-service-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-dlq-service-live-test COMMAND guard-dlq-service-live-test)

# Opt-in live check against a real NATS; skips without ARGUS_NATS_URL.
add_executable(guard-dlq-live-test tests/unit/guard-dlq-live-test.cc)
set_target_properties(guard-dlq-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-dlq-live-test PRIVATE argus::nats doctest::doctest)
target_compile_options(guard-dlq-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-dlq-live-test COMMAND guard-dlq-live-test)

# Opt-in live check against a real argus-vlm; skips without the env vars.
add_executable(vlm-client-live-test tests/unit/vlm-client-live-test.cc)
set_target_properties(vlm-client-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(vlm-client-live-test PRIVATE
    argus::vlm-client
    argus::access
    argus::auth
    argus::config
    argus::hash
    argus::json
    argus::nats
    argus::sqlite
    argus::storage
    argus::threading
    argus::validation
    pkg::response
    Drogon::Drogon
    doctest::doctest)
target_include_directories(vlm-client-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_options(vlm-client-live-test PRIVATE -Wall -Wextra)
add_test(NAME vlm-client-live-test COMMAND vlm-client-live-test)

# Opt-in live check: fake camera RPC + real VLM exercise the assessment path.
add_executable(guard-assessment-live-test
    tests/unit/guard-assessment-live-test.cc)
set_target_properties(guard-assessment-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-assessment-live-test PRIVATE
    argus::guard
    argus::vlm-client
    argus::client-camera-actions
    argus::access
    argus::auth
    argus::config
    argus::hash
    argus::json
    argus::nats
    argus::sqlite
    argus::storage
    argus::threading
    argus::validation
    pkg::response
    Drogon::Drogon
    doctest::doctest)
target_include_directories(guard-assessment-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_options(guard-assessment-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-assessment-live-test COMMAND guard-assessment-live-test)

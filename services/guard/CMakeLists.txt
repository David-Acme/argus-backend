cmake_minimum_required(VERSION 3.25)
project(argus-guard VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(GUARD_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenSSL REQUIRED)
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
    if(NOT TARGET argus::lib::storage)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/storage
                         ${CMAKE_BINARY_DIR}/storage)
    endif()
    if(NOT TARGET argus::contracts::camera)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/camera
                         ${CMAKE_BINARY_DIR}/contracts/camera)
    endif()
    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()
    if(NOT TARGET argus::clients::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                         ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::clients::guard)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/guard
                         ${CMAKE_BINARY_DIR}/clients/guard EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::clients::notification)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                         ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::clients::camera-actions)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera-actions
                         ${CMAKE_BINARY_DIR}/clients/camera-actions EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::clients::llm)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm
                         ${CMAKE_BINARY_DIR}/clients/llm)
    endif()
    if(NOT TARGET argus::clients::vlm)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/vlm
                         ${CMAKE_BINARY_DIR}/clients/vlm)
    endif()
endif()

if(NOT COMMAND argus_settings_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/settings
                     ${CMAKE_BINARY_DIR}/contracts/settings)
endif()

find_package(doctest REQUIRED)

add_subdirectory(${GUARD_SRC_ROOT}/config)
add_subdirectory(${GUARD_SRC_ROOT}/shared/repositories/presence)

file(GLOB GUARD_FEATURE_MODULES CONFIGURE_DEPENDS
    "${GUARD_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS GUARD_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()
add_subdirectory(${GUARD_SRC_ROOT}/app/rpc)

argus_service(NAME argus-guard
    MAIN ${GUARD_SRC_ROOT}/app/main.cc
    MODULES
        argus::guard
        argus::guard-config
        argus::guard-presence
        argus::guard-rpc
        argus::guard-safety
        argus::guard-settings
        argus::guard-module-data
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::config
        argus::lib::http
        argus::lib::nats
        argus::lib::sqlite
        argus::lib::text
        argus::clients::guard
        argus::clients::identity
        argus::clients::notification
        argus::clients::camera-actions
        argus::clients::llm
        argus::clients::vlm
    PORTS 7039)

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
        guard-belief-scope-test guard-outbox-adopt-test
        guard-vocabulary-test guard-schedule-test guard-scenario-test
        guard-episode-test guard-copy-test guard-context-test
        guard-presence-repository-test guard-response-test
        guard-module-wind-down-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)

    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)

    target_link_libraries(${TEST_NAME} PRIVATE argus::guard
        argus::guard-presence-repository doctest::doctest)
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    if(TEST_NAME STREQUAL "guard-schema-test"
       OR TEST_NAME STREQUAL "guard-migration-test"
       OR TEST_NAME STREQUAL "guard-saga-test"
       OR TEST_NAME STREQUAL "guard-retry-lifecycle-test"
       OR TEST_NAME STREQUAL "guard-dialogue-service-test"
       OR TEST_NAME STREQUAL "guard-decision-journal-test"
       OR TEST_NAME STREQUAL "guard-notify-thread-test"
       OR TEST_NAME STREQUAL "guard-belief-scope-test"
       OR TEST_NAME STREQUAL "guard-outbox-adopt-test"
       OR TEST_NAME STREQUAL "guard-dlq-crash-test"
       OR TEST_NAME STREQUAL "guard-scenario-test"
       OR TEST_NAME STREQUAL "guard-episode-test"
       OR TEST_NAME STREQUAL "guard-presence-repository-test"
       OR TEST_NAME STREQUAL "guard-response-test"
       OR TEST_NAME STREQUAL "guard-module-wind-down-test")
        target_compile_definitions(${TEST_NAME} PRIVATE
            ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
    endif()
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

add_executable(guard-presence-test tests/unit/guard-presence-test.cc)
set_target_properties(guard-presence-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-presence-test PRIVATE
    argus::guard
    argus::guard-presence
    argus::guard-rpc
    argus::clients::guard
    doctest::doctest)
target_include_directories(guard-presence-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_definitions(guard-presence-test PRIVATE
    ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(guard-presence-test PRIVATE -Wall -Wextra)
add_test(NAME guard-presence-test COMMAND guard-presence-test)

add_executable(guard-settings-test tests/unit/guard-settings-test.cc)
set_target_properties(guard-settings-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-settings-test PRIVATE
    argus::guard
    argus::guard-config
    argus::guard-settings
    argus::contracts::settings-wire
    doctest::doctest)
target_compile_options(guard-settings-test PRIVATE -Wall -Wextra)
argus_runtime_rpath(guard-settings-test)
add_test(NAME guard-settings-test COMMAND guard-settings-test)

add_executable(guard-dlq-service-live-test
    tests/unit/guard-dlq-service-live-test.cc)
set_target_properties(guard-dlq-service-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-dlq-service-live-test PRIVATE
    argus::guard
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)
target_include_directories(guard-dlq-service-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_definitions(guard-dlq-service-live-test PRIVATE
    ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(guard-dlq-service-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-dlq-service-live-test COMMAND guard-dlq-service-live-test)

add_executable(guard-encounter-drain-live-test
    tests/unit/guard-encounter-drain-live-test.cc)
set_target_properties(guard-encounter-drain-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-encounter-drain-live-test PRIVATE
    argus::guard
    argus::lib::nats
    argus::lib::sqlite
    doctest::doctest)
target_include_directories(guard-encounter-drain-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_definitions(guard-encounter-drain-live-test PRIVATE
    ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(guard-encounter-drain-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-encounter-drain-live-test
         COMMAND guard-encounter-drain-live-test)

add_executable(guard-verdict-live-test tests/unit/guard-verdict-live-test.cc)
set_target_properties(guard-verdict-live-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-verdict-live-test PRIVATE
    argus::guard
    argus::guard-presence-repository
    doctest::doctest)
target_include_directories(guard-verdict-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_definitions(guard-verdict-live-test PRIVATE
    ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(guard-verdict-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-verdict-live-test COMMAND guard-verdict-live-test)

add_executable(guard-dlq-live-test tests/unit/guard-dlq-live-test.cc)
set_target_properties(guard-dlq-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-dlq-live-test PRIVATE argus::lib::nats doctest::doctest)
target_compile_options(guard-dlq-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-dlq-live-test COMMAND guard-dlq-live-test)

add_executable(vlm-client-live-test tests/unit/vlm-client-live-test.cc)
set_target_properties(vlm-client-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(vlm-client-live-test PRIVATE
    argus::clients::vlm
    argus::lib::auth
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::storage
    argus::lib::runtime
    argus::lib::validation
    argus::lib::http
    Drogon::Drogon
    doctest::doctest)
target_include_directories(vlm-client-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_options(vlm-client-live-test PRIVATE -Wall -Wextra)
add_test(NAME vlm-client-live-test COMMAND vlm-client-live-test)

add_executable(guard-assessment-live-test
    tests/unit/guard-assessment-live-test.cc)
set_target_properties(guard-assessment-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-assessment-live-test PRIVATE
    argus::guard
    argus::clients::vlm
    argus::clients::camera-actions
    argus::lib::auth
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::storage
    argus::lib::runtime
    argus::lib::validation
    argus::lib::http
    Drogon::Drogon
    doctest::doctest)
target_include_directories(guard-assessment-live-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_options(guard-assessment-live-test PRIVATE -Wall -Wextra)
add_test(NAME guard-assessment-live-test COMMAND guard-assessment-live-test)

add_executable(guard-safety-test tests/unit/guard-safety-test.cc)
set_target_properties(guard-safety-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-safety-test PRIVATE
    argus::guard
    argus::guard-safety
    argus::guard-module-data
    argus::guard-presence-repository
    doctest::doctest)
target_include_directories(guard-safety-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_options(guard-safety-test PRIVATE -Wall -Wextra)
target_compile_definitions(guard-safety-test PRIVATE
    ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
add_test(NAME guard-safety-test COMMAND guard-safety-test)

add_executable(guard-module-data-test tests/unit/guard-module-data-test.cc)
set_target_properties(guard-module-data-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
target_link_libraries(guard-module-data-test PRIVATE
    argus::guard
    argus::guard-module-data
    doctest::doctest)
target_include_directories(guard-module-data-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
target_compile_options(guard-module-data-test PRIVATE -Wall -Wextra)
target_compile_definitions(guard-module-data-test PRIVATE
    ARGUS_GUARD_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
argus_runtime_rpath(guard-module-data-test)
add_test(NAME guard-module-data-test COMMAND guard-module-data-test)

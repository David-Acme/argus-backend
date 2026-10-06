cmake_minimum_required(VERSION 3.25)
project(argus-productivity VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(PRODUCTIVITY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
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

    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()

    if(NOT TARGET argus::contracts::productivity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/productivity
                         ${CMAKE_BINARY_DIR}/contracts/productivity)
    endif()

    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()

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

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::productivity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/productivity
                     ${CMAKE_BINARY_DIR}/clients/productivity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::lib::grpc-health)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/grpc
                     ${CMAKE_BINARY_DIR}/grpc EXCLUDE_FROM_ALL)
endif()

add_subdirectory(src/config)
add_subdirectory(src/shared/services/change-sink)
add_subdirectory(src/shared/dtos)
add_subdirectory(src/shared/repositories)

file(GLOB PRODUCTIVITY_FEATURE_MODULES CONFIGURE_DEPENDS
    "${PRODUCTIVITY_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS PRODUCTIVITY_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/app/rpc)

argus_service(NAME argus-productivity
    MAIN ${PRODUCTIVITY_SRC_ROOT}/app/main.cc
    MODULES
        argus::productivity-config
        argus::productivity-change-sink
        argus::productivity-calendar-event
        argus::productivity-calendar-event-share
        argus::productivity-project
        argus::productivity-project-member
        argus::productivity-project-task
        argus::productivity-reminder
        argus::productivity-sync
        argus::productivity-agenda
        argus::productivity-module-data
        argus::productivity-rpc-server
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::http
        argus::lib::validation
        argus::clients::productivity
    PORTS 7027 7037)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

if(PROJECT_IS_TOP_LEVEL AND NOT TARGET productivity-migration)
    add_subdirectory(tools/migrate-productivity)
endif()

foreach(TEST_NAME IN ITEMS productivity-schema-test productivity-migration-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
        target_link_libraries(${TEST_NAME} PRIVATE
    argus::lib::auth
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::validation
    argus::contracts::sync
    argus::lib::http
    productivity-migration
    doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

add_executable(productivity-change-outbox-sink-test
    tests/unit/productivity-change-outbox-sink-test.cc)

set_target_properties(productivity-change-outbox-sink-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(productivity-change-outbox-sink-test PRIVATE
    argus::productivity-change-sink
    argus::lib::nats
    doctest::doctest)

target_compile_definitions(productivity-change-outbox-sink-test PRIVATE
    ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(productivity-change-outbox-sink-test
                       PRIVATE -Wall -Wextra)

add_test(NAME productivity-change-outbox-sink-test
         COMMAND productivity-change-outbox-sink-test)

add_executable(productivity-controller-test
    tests/unit/productivity-controller-test.cc)

set_target_properties(productivity-controller-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_link_libraries(productivity-controller-test PRIVATE
    argus::productivity-calendar-event
    argus::productivity-calendar-event-share
    argus::productivity-project
    argus::productivity-project-member
    argus::productivity-project-task
    argus::clients::identity
    argus::lib::auth
    argus::contracts::auth
    argus::contracts::productivity
    argus::contracts::sync
    argus::lib::config
    argus::lib::text
    argus::lib::sqlite
    argus::lib::validation
    argus::lib::http
    doctest::doctest)

target_compile_options(productivity-controller-test PRIVATE -Wall -Wextra)

add_test(NAME productivity-controller-test
         COMMAND productivity-controller-test)

add_executable(productivity-change-transaction-test
    tests/unit/productivity-change-transaction-test.cc)

set_target_properties(productivity-change-transaction-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(productivity-change-transaction-test PRIVATE
    ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(productivity-change-transaction-test PRIVATE
    argus::productivity-calendar-event
    argus::productivity-calendar-event-share
    argus::productivity-project
    argus::productivity-project-member
    argus::productivity-project-task
    argus::productivity-change-sink
    argus::lib::auth
    argus::contracts::auth
    argus::contracts::productivity
    argus::contracts::sync
    argus::lib::config
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::validation
    argus::lib::http
    doctest::doctest)

target_compile_options(productivity-change-transaction-test
                       PRIVATE -Wall -Wextra)

add_test(NAME productivity-change-transaction-test
         COMMAND productivity-change-transaction-test)

add_executable(productivity-agenda-test
    tests/unit/productivity-agenda-test.cc)

set_target_properties(productivity-agenda-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(productivity-agenda-test PRIVATE
    ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(productivity-agenda-test PRIVATE
    argus::productivity-agenda
    argus::lib::sqlite
    doctest::doctest)

target_compile_options(productivity-agenda-test PRIVATE -Wall -Wextra)

add_test(NAME productivity-agenda-test COMMAND productivity-agenda-test)

add_executable(productivity-sync-rpc-test
    tests/unit/productivity-sync-rpc-test.cc)

set_target_properties(productivity-sync-rpc-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_definitions(productivity-sync-rpc-test PRIVATE
    ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(productivity-sync-rpc-test PRIVATE
    argus::productivity-sync
    argus::clients::productivity
    argus::lib::sqlite
    argus::contracts::sync
    doctest::doctest)

target_compile_options(productivity-sync-rpc-test PRIVATE -Wall -Wextra)

add_test(NAME productivity-sync-rpc-test
         COMMAND productivity-sync-rpc-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(productivity-module-data-test
    tests/unit/productivity-module-data-test.cc)
set_target_properties(productivity-module-data-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_compile_definitions(productivity-module-data-test PRIVATE
    ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_link_libraries(productivity-module-data-test PRIVATE
    argus::productivity-module-data
    argus::lib::sqlite
    doctest::doctest)
target_compile_options(productivity-module-data-test PRIVATE -Wall -Wextra)
add_test(NAME productivity-module-data-test
         COMMAND productivity-module-data-test)

add_executable(productivity-reminder-test
    tests/unit/productivity-reminder-test.cc)
set_target_properties(productivity-reminder-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
target_compile_definitions(productivity-reminder-test PRIVATE
    ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_link_libraries(productivity-reminder-test PRIVATE
    argus::productivity-reminder
    argus::productivity-repositories
    argus::clients::productivity
    argus::lib::auth
    argus::lib::config
    argus::lib::sqlite
    argus::lib::validation
    argus::contracts::sync
    doctest::doctest)
target_compile_options(productivity-reminder-test PRIVATE -Wall -Wextra)
add_test(NAME productivity-reminder-test COMMAND productivity-reminder-test)

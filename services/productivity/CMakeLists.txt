cmake_minimum_required(VERSION 3.25)
project(argus-productivity VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(PRODUCTIVITY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    # Standalone configure: this service owns the whole build, so it adds the
    # packages it links — the leaves it uses directly and the sibling packages
    # that used to arrive through argus-common — plus the third-party
    # sqlite-vec, so the productivity service links the same objects as any other
    # consumer.
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
    if(NOT TARGET argus::lib::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/cert
                         ${CMAKE_BINARY_DIR}/cert)
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
endif()

find_package(doctest REQUIRED)

# The productivity sync contract (argus.productivity.v1.SyncService); this
# service is the owner and serves it, consumers link argus::clients::productivity.
if(NOT TARGET argus::clients::productivity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/productivity
                     ${CMAKE_BINARY_DIR}/clients/productivity EXCLUDE_FROM_ALL)
endif()

# This service owns the productivity tables' row code since sub-step 3a-1b: the
# repositories, their schemas and the NATS change sink compile here, and the
# sync RPC owner reads the same definitions through this library.
add_library(productivity-core STATIC
    ${PRODUCTIVITY_SRC_ROOT}/productivity/nats-productivity-change-sink.cc
    ${PRODUCTIVITY_SRC_ROOT}/productivity/productivity-config.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/repositories/calendar-event/calendar-event-repository.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/repositories/calendar-event-share/calendar-event-share-repository.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/repositories/project/project-repository.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/repositories/project-member/project-member-repository.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/repositories/project-task/project-task-repository.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/repositories/reminder/reminder-repository.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/repositories/reminder-detail/reminder-detail-repository.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/schemas/calendar-event/calendar-event-schema.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/schemas/calendar-event-share/calendar-event-share-schema.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/schemas/project/project-schema.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/schemas/project-member/project-member-schema.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/schemas/project-task/project-task-schema.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/schemas/reminder/reminder-schema.cc
    ${PRODUCTIVITY_SRC_ROOT}/shared/schemas/reminder-detail/reminder-detail-schema.cc
)

target_include_directories(productivity-core PUBLIC
    ${PRODUCTIVITY_SRC_ROOT})

target_link_libraries(productivity-core PUBLIC
    argus::contracts::productivity
    argus::contracts::sync
    argus::lib::nats
    argus::lib::text
    argus::lib::sqlite
    ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(productivity-core PRIVATE -Wall -Wextra)

# The write-side feature surface (calendar-event, calendar-event-share,
# project, project-member, project-task controllers + services + DTOs)
# compiles from the shared tree into THIS executable only: the controllers are
# AutoCreation and register their routes during static init, exactly like the
# legacy binary does. The legacy keeps its own registration until F3-2; the
# repositories and schemas are productivity-core's.
set(PRODUCTIVITY_FEATURE_SOURCES
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event/controllers/calendar-event-controller.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event/dtos/create-calendar-event-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event/dtos/update-calendar-event-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event/services/calendar-event-feature-service.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event-share/controllers/calendar-event-share-controller.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event-share/dtos/create-calendar-event-share-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event-share/dtos/update-calendar-event-share-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/calendar-event-share/services/calendar-event-share-feature-service.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project/controllers/project-controller.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project/dtos/create-project-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project/dtos/update-project-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project/services/project-feature-service.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-member/controllers/project-member-controller.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-member/dtos/create-project-member-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-member/dtos/update-project-member-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-member/services/project-member-feature-service.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-task/controllers/project-task-controller.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-task/dtos/create-project-task-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-task/dtos/update-project-task-dto.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/api/project-task/services/project-task-feature-service.cc
)

add_executable(argus-productivity
    ${PRODUCTIVITY_SRC_ROOT}/main.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/sync/productivity-sync-rpc-service.cc
    ${PRODUCTIVITY_FEATURE_SOURCES})

# The filters validate through argus.identity.v1 (argus::clients::identity rides
# argus::lib::auth); the sync RPC owner serves argus.productivity.v1 via
# argus::clients::productivity. The DTOs validate through argus::lib::validation
# and the controllers answer through argus::lib::http.
target_link_libraries(argus-productivity PRIVATE productivity-core argus::contracts::productivity argus::contracts::sync
    argus::lib::auth argus::lib::validation argus::lib::http argus::clients::productivity)

target_compile_options(argus-productivity PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# productivity-migration builds only as a dependency of the migration suite.
# The standalone build adds its folder here; the root tree finds it via the
# discovery glob.
if(PROJECT_IS_TOP_LEVEL AND NOT TARGET productivity-migration)
    add_subdirectory(tools/migrate-productivity)
endif()

# The unit suites opt back into the default build: this folder is added
# EXCLUDE_FROM_ALL, and productivity-migration (tools/) stays out of it.
find_package(doctest REQUIRED)

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

# The controller suite compiles the same write-side feature sources the
# service compiles and rides productivity-core for the repositories, schemas
# and boot plumbing.
add_executable(productivity-controller-test
    tests/unit/productivity-controller-test.cc
    ${PRODUCTIVITY_FEATURE_SOURCES})

set_target_properties(productivity-controller-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(productivity-controller-test PRIVATE
    ${PRODUCTIVITY_SRC_ROOT})

target_link_libraries(productivity-controller-test PRIVATE
    productivity-core
    argus_identity
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

# The sync RPC suite boots the real schema and the real service against it.
add_executable(productivity-sync-rpc-test
    tests/unit/productivity-sync-rpc-test.cc
    ${PRODUCTIVITY_SRC_ROOT}/feature/sync/productivity-sync-rpc-service.cc)

set_target_properties(productivity-sync-rpc-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(productivity-sync-rpc-test PRIVATE
    ${PRODUCTIVITY_SRC_ROOT})

target_compile_definitions(productivity-sync-rpc-test PRIVATE
    ARGUS_PRODUCTIVITY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_link_libraries(productivity-sync-rpc-test PRIVATE
    productivity-core
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

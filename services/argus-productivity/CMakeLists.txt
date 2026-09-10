cmake_minimum_required(VERSION 3.25)
project(argus-productivity VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(PRODUCTIVITY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET argus_common)
    # Standalone build: reuse the root project's libraries and third-party
    # sqlite-vec so the productivity service links the same objects as the
    # root-tree build.
    # The substrate still finds the vendored gRPC and would build the cq
    # bridge, whose entry symbol then interposes the identically-named
    # implementation inside libgrpc. Empty stand-ins skip it.
    if(NOT TARGET argus_sdk_grpc_bridge_entry)
        add_library(argus_sdk_grpc_bridge_entry INTERFACE)
        add_library(argus_sdk_grpc_bridge_exit INTERFACE)
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

    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-common
                     ${CMAKE_BINARY_DIR}/argus-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-sqlite
                     ${CMAKE_BINARY_DIR}/argus-sqlite)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-audit
                     ${CMAKE_BINARY_DIR}/argus-audit)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-cert
                     ${CMAKE_BINARY_DIR}/argus-cert)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-room
                     ${CMAKE_BINARY_DIR}/argus-room)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-socket
                     ${CMAKE_BINARY_DIR}/argus-socket)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-auth
                     ${CMAKE_BINARY_DIR}/argus-auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-identity
                     ${CMAKE_BINARY_DIR}/argus-identity)
    if(NOT TARGET identity-migration)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-identity/tools/migrate-identity
                         ${CMAKE_BINARY_DIR}/argus-identity/tools/migrate-identity)
    endif()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-sync
                     ${CMAKE_BINARY_DIR}/argus-sync)
endif()

find_package(doctest REQUIRED)

add_library(productivity-core STATIC
    ${PRODUCTIVITY_SRC_ROOT}/productivity/nats-productivity-change-sink.cc
    ${PRODUCTIVITY_SRC_ROOT}/productivity/productivity-config.cc
)

target_include_directories(productivity-core PUBLIC
    ${PRODUCTIVITY_SRC_ROOT})

target_link_libraries(productivity-core PUBLIC argus_sync argus::sqlite
    ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(productivity-core PRIVATE -Wall -Wextra)

# The write-side feature surface (calendar-event, calendar-event-share,
# project, project-member, project-task controllers + services + DTOs)
# compiles from the shared tree into THIS executable only: the controllers are
# AutoCreation and register their routes during static init, exactly like the
# legacy binary does. The legacy keeps its own registration until F3-2; the
# repositories and schemas ride argus_sync.
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
    ${PRODUCTIVITY_FEATURE_SOURCES})

# The filters validate through argus.identity.v1 (argus::sdk-identity rides
# argus::auth); argus_identity stays for the share/member reads and arrives
# transitively through productivity-core's argus_sync pull.
target_link_libraries(argus-productivity PRIVATE productivity-core
    argus::auth)

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
enable_testing()
find_package(doctest REQUIRED)

foreach(TEST_NAME IN ITEMS productivity-schema-test productivity-migration-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
    target_link_libraries(${TEST_NAME} PRIVATE argus_common
        productivity-migration doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# The controller suite compiles the same write-side feature sources the
# service compiles and rides the sync layer for the repositories,
# schemas and boot plumbing.
add_executable(productivity-controller-test
    tests/unit/productivity-controller-test.cc
    ${PRODUCTIVITY_FEATURE_SOURCES})

set_target_properties(productivity-controller-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_include_directories(productivity-controller-test PRIVATE
    ${PRODUCTIVITY_SRC_ROOT})

target_link_libraries(productivity-controller-test PRIVATE argus_sync
    argus_common doctest::doctest)

target_compile_options(productivity-controller-test PRIVATE -Wall -Wextra)

add_test(NAME productivity-controller-test
         COMMAND productivity-controller-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

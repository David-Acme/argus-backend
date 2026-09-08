cmake_minimum_required(VERSION 3.25)
project(argus-productivity VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PRODUCTIVITY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET argus_common)
    # Standalone build: reuse the root project's libraries and third-party
    # sqlite-vec so the productivity service links the same objects as the
    # root-tree build.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-common
                     ${CMAKE_BINARY_DIR}/argus-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/sqlite
                     ${CMAKE_BINARY_DIR}/argus-sqlite)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/audit
                     ${CMAKE_BINARY_DIR}/argus-audit)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/cert
                     ${CMAKE_BINARY_DIR}/argus-cert)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/room
                     ${CMAKE_BINARY_DIR}/argus-room)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/socket
                     ${CMAKE_BINARY_DIR}/argus-socket)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-auth
                     ${CMAKE_BINARY_DIR}/argus-auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-identity
                     ${CMAKE_BINARY_DIR}/argus-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/sync
                     ${CMAKE_BINARY_DIR}/argus-sync)

    enable_language(C)
    add_library(sqlite-vec STATIC
                ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec/sqlite-vec.c)
    target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
    target_include_directories(sqlite-vec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec)
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
    set(NCNN_ENABLE_PIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ncnn
                     ${CMAKE_BINARY_DIR}/third_party/ncnn EXCLUDE_FROM_ALL)
    set(ARGUS_NCNN_TARGET ncnn)
endif()

find_package(doctest REQUIRED)

add_library(productivity-core STATIC
    ${PRODUCTIVITY_SRC_ROOT}/productivity/nats-productivity-change-sink.cc
    ${PRODUCTIVITY_SRC_ROOT}/productivity/productivity-config.cc
)

target_include_directories(productivity-core PUBLIC
    ${PRODUCTIVITY_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(productivity-core PUBLIC argus_sync argus::sqlite
    ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(productivity-core PRIVATE -Wall -Wextra)

# The write-side feature surface (calendar-event, calendar-event-share,
# project, project-member, project-task controllers + services + DTOs)
# compiles from the shared tree into THIS executable only: the controllers are
# AutoCreation and register their routes during static init, exactly like the
# legacy binary does. The legacy keeps its own registration until F3-2; the
# repositories and schemas ride argus_sync.
add_executable(argus-productivity
    ${PRODUCTIVITY_SRC_ROOT}/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event/controllers/calendar-event-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event/dtos/create-calendar-event-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event/dtos/update-calendar-event-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event/services/calendar-event-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event-share/controllers/calendar-event-share-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event-share/dtos/create-calendar-event-share-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event-share/dtos/update-calendar-event-share-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/calendar-event-share/services/calendar-event-share-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project/controllers/project-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project/dtos/create-project-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project/dtos/update-project-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project/services/project-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-member/controllers/project-member-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-member/dtos/create-project-member-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-member/dtos/update-project-member-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-member/services/project-member-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-task/controllers/project-task-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-task/dtos/create-project-task-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-task/dtos/update-project-task-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/project-task/services/project-task-feature-service.cc
)

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

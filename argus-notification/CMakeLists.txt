cmake_minimum_required(VERSION 3.25)
project(argus-notification VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NOTIFICATION_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET argus_common)
    # Standalone build: reuse the root project's libraries and third-party
    # sqlite-vec so the notification service links the same objects as the
    # root-tree build.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    find_package(mdns REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-common
                     ${CMAKE_BINARY_DIR}/argus-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/sqlite
                     ${CMAKE_BINARY_DIR}/argus-sqlite)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/audit
                     ${CMAKE_BINARY_DIR}/argus-audit)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/filter
                     ${CMAKE_BINARY_DIR}/argus-filter)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/identity
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

add_library(notification-core STATIC
    ${NOTIFICATION_SRC_ROOT}/controllers/health-controller.cc
    ${NOTIFICATION_SRC_ROOT}/notification/nats-notification-change-sink.cc
    ${NOTIFICATION_SRC_ROOT}/notification/notification-config.cc
    ${NOTIFICATION_SRC_ROOT}/server/listener-config.cc
)

target_include_directories(notification-core PUBLIC
    ${NOTIFICATION_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(notification-core PUBLIC argus_sync argus::sqlite
    ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(notification-core PRIVATE -Wall -Wextra)

# The write-side feature surface (markAsRead + registerToken controllers,
# feature services, DTOs) compiles from the shared tree into THIS executable
# only: the controllers are AutoCreation and register their routes during
# static init, exactly like the legacy binary does. The legacy keeps its own
# registration until F3-2; the notification repository and schema ride
# argus_sync, the notification-token repository/service are not in argus_sync
# and compile from the shared tree too.
add_executable(argus-notification
    ${NOTIFICATION_SRC_ROOT}/main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/notification/controllers/notification-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/notification/controllers/notification-token-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/notification/dtos/notification-read-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/notification/dtos/register-notification-token-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/notification/services/notification-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/notification/services/notification-token-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/repositories/notification-token/notification-token-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/schemas/notification-token/notification-token-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/notification-token/notification-token-service.cc
)

# argus::auth linked ahead of the transitive argus_identity pull: main.cc
# constructs the filters directly (DeviceFilter/JwtFilter/RoleFilter/
# ValidJsonFilter), and those filters read back into argus_identity's
# UserRepository/RefreshTokenRepository/DeviceCredentialRepository, so a
# single-pass linker needs argus::auth resolved before it, not after.
target_link_libraries(argus-notification PRIVATE notification-core
    argus::auth argus_identity)

target_compile_options(argus-notification PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

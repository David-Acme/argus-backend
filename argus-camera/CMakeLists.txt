cmake_minimum_required(VERSION 3.25)
project(argus-camera VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CAMERA_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET argus_common)
    # Standalone build: reuse the root project's libraries and third-party
    # sqlite-vec so the camera service links the same objects as the
    # root-tree build.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    find_package(mdns REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/shared
                     ${CMAKE_BINARY_DIR}/argus-common)
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

# ncnn is THE objects capacity of this service (F2-3): the detector is the
# only AI linkage here — no face/llm/vlm/tts/stt/vad code is compiled.
set(NCNN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ncnn/src)

add_library(camera-core STATIC
    ${CAMERA_SRC_ROOT}/controllers/health-controller.cc
    ${CAMERA_SRC_ROOT}/controllers/camera-media-service.cc
    ${CAMERA_SRC_ROOT}/camera/camera-config.cc
    ${CAMERA_SRC_ROOT}/camera/nats-camera-change-sink.cc
    ${CAMERA_SRC_ROOT}/server/listener-config.cc
    ${CAMERA_SRC_ROOT}/objects/ncnn-object-detector.cc
    ${CAMERA_SRC_ROOT}/operator/camera-operator-service.cc
    ${CAMERA_SRC_ROOT}/operator/event-intelligence.cc
    ${CAMERA_SRC_ROOT}/operator/go2rtc-frame-source.cc
    ${CAMERA_SRC_ROOT}/operator/nats-object-event-sink.cc
    ${CAMERA_SRC_ROOT}/operator/object-event.cc
    ${CAMERA_SRC_ROOT}/operator/operator-config.cc
    # Boot plumbing the service shares with the backend: the centralized
    # response config (envelope/CORS/404/405) and the SQLite layer (DbService
    # with the named identity client slot).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/app-config.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/sqlite/db-service.cc
    # Camera-domain CRUD compiled from the shared tree so the legacy binary
    # and this service serve byte-identical contracts.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/camera/controllers/camera-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/camera/dtos/create-camera-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/camera/dtos/update-camera-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/camera/services/camera-feature-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/zone/controllers/zone-controller.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/zone/dtos/create-zone-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/zone/dtos/update-zone-dto.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/feature/api/zone/services/zone-feature-service.cc
    # Stream lifecycle: go2rtc supervision and the fMP4 StreamHub relay.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/stream/go2rtc-manager.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/stream/stream-hub.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/stream/upstream-http.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/stream/ws-frame.cc
)

target_include_directories(camera-core SYSTEM PUBLIC
    ${NCNN_SRC_DIR}
    ${CMAKE_BINARY_DIR}/third_party/ncnn/src)

target_include_directories(camera-core PUBLIC
    ${CAMERA_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(camera-core PUBLIC argus_sync sqlite-vec opencv::opencv ${ARGUS_NCNN_TARGET})

target_compile_options(camera-core PRIVATE -Wall -Wextra)

add_executable(argus-camera ${CAMERA_SRC_ROOT}/main.cc)

target_link_libraries(argus-camera PRIVATE camera-core)

target_compile_options(argus-camera PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# Verification labs, built on demand (--target argus-object-bench /
# argus-camera-probe); never part of the default service build.
add_subdirectory(labs/object-bench EXCLUDE_FROM_ALL)
add_subdirectory(labs/camera-probe EXCLUDE_FROM_ALL)

# The camera suites (schema, migration, config) run from src/test/unit so the
# root ctest covers them without a standalone conan gate.

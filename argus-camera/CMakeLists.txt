cmake_minimum_required(VERSION 3.25)
project(argus-camera VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CAMERA_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET argus_common)
    # Standalone build: reuse the root project's argus_common and third-party
    # sqlite-vec so the camera service links the same objects as the root-tree
    # build.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/shared
                     ${CMAKE_BINARY_DIR}/argus-common)

    enable_language(C)
    add_library(sqlite-vec STATIC
                ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec/sqlite-vec.c)
    target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
    target_include_directories(sqlite-vec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec)
endif()

find_package(doctest REQUIRED)

add_library(camera-core STATIC
    ${CAMERA_SRC_ROOT}/controllers/health-controller.cc
    ${CAMERA_SRC_ROOT}/camera/camera-config.cc
    ${CAMERA_SRC_ROOT}/server/listener-config.cc
    # Boot plumbing the service shares with the backend: the centralized
    # response config (envelope/CORS/404/405) and the SQLite layer (DbService
    # with the named identity client slot).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/app-config.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/sqlite/db-service.cc
)

target_include_directories(camera-core PUBLIC
    ${CAMERA_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(camera-core PUBLIC argus_common sqlite-vec)

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

# Test registration is standalone-only so the root project's ctest stays at
# its suite count (the camera suites run from src/test/unit instead).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
endif()

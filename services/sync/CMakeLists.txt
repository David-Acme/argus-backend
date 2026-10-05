cmake_minimum_required(VERSION 3.25)
project(argus-sync-service VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(SYNC_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::contracts::voice)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/voice
                         ${CMAKE_BINARY_DIR}/contracts/voice)
    endif()
    if(NOT TARGET argus::contracts::camera)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/camera
                         ${CMAKE_BINARY_DIR}/contracts/camera)
    endif()
    if(NOT TARGET argus::contracts::notification)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/notification
                         ${CMAKE_BINARY_DIR}/contracts/notification)
    endif()
    if(NOT TARGET argus::contracts::productivity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/productivity
                         ${CMAKE_BINARY_DIR}/contracts/productivity)
    endif()
endif()

if(NOT TARGET argus::clients::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/sync
                     ${CMAKE_BINARY_DIR}/clients/sync EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::clients::camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera
                     ${CMAKE_BINARY_DIR}/clients/camera EXCLUDE_FROM_ALL)
endif()
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
if(NOT TARGET argus::clients::voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/voice
                     ${CMAKE_BINARY_DIR}/clients/voice EXCLUDE_FROM_ALL)
endif()

find_package(doctest REQUIRED)

file(GLOB SYNC_FEATURE_MODULES CONFIGURE_DEPENDS
    "${SYNC_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS SYNC_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/config)
add_subdirectory(src/app/rpc)
add_subdirectory(src/shared/repositories)
add_subdirectory(src/shared/services)

argus_service(NAME argus-sync
    MAIN ${SYNC_SRC_ROOT}/app/main.cc
    MODULES argus::sync-config
            argus::sync-transport
            argus::sync-fanout
            argus::sync-rtc
            argus::sync-heartbeat
            argus::sync-control-rpc
            argus::sync-repositories
            argus::sync-services
    DEPENDS argus::lib::http
            argus::lib::auth
            argus::clients::sync
            Drogon::Drogon
    PORTS 7025 7041)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_subdirectory(tools/migrate-sync)

add_subdirectory(tests)

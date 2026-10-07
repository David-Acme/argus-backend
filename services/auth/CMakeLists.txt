cmake_minimum_required(VERSION 3.25)
project(argus-auth-service VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::test-support)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/test-support
                     ${CMAKE_BINARY_DIR}/test-support)
endif()

set(AUTH_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)

    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)

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
    if(NOT TARGET argus::lib::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()
    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()
endif()

if(NOT TARGET argus::clients::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/auth
                     ${CMAKE_BINARY_DIR}/clients/auth)
endif()

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity)
endif()

find_package(doctest REQUIRED)

file(GLOB AUTH_FEATURE_MODULES CONFIGURE_DEPENDS
    "${AUTH_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS AUTH_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/config)
add_subdirectory(src/app/rpc)

argus_service(NAME argus-auth
    MAIN ${AUTH_SRC_ROOT}/app/main.cc
    MODULES argus::auth-config
            argus::auth-auth
            argus::auth-session
            argus::auth-device
            argus::auth-rpc
    DEPENDS argus::lib::http
            argus::lib::auth
            argus::lib::nats
            argus::contracts::auth
            argus::clients::identity
            Drogon::Drogon
    PORTS 7042 7043)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_subdirectory(tools/migrate-auth)
add_subdirectory(tests)

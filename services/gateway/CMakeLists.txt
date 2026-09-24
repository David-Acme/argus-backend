cmake_minimum_required(VERSION 3.25)
project(argus-gateway VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(GATEWAY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
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
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()

    if(NOT TARGET argus::lib::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/cert
                         ${CMAKE_BINARY_DIR}/cert)
    endif()
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
endif()

if(NOT TARGET argus::clients::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/sync
                     ${CMAKE_BINARY_DIR}/clients/sync EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET argus::lib::mdns)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/mdns
                     ${CMAKE_BINARY_DIR}/mdns)
endif()

find_package(doctest REQUIRED)

if(NOT TARGET argus::contracts::gateway)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/gateway
                     ${CMAKE_BINARY_DIR}/contracts/gateway)
endif()

add_library(gateway-core STATIC
    ${GATEWAY_SRC_ROOT}/proxy/proxy-config.cc
    ${GATEWAY_SRC_ROOT}/proxy/reverse-proxy.cc
    ${GATEWAY_SRC_ROOT}/server/remote-config.cc
    ${GATEWAY_SRC_ROOT}/server/remote-gate.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-stream-relay.cc
    ${GATEWAY_SRC_ROOT}/sync/camera-stream-socket.cc
)

target_include_directories(gateway-core PUBLIC ${GATEWAY_SRC_ROOT})

target_link_libraries(gateway-core PUBLIC
    argus::lib::auth argus::lib::cert argus::lib::mdns argus::lib::http
    argus::contracts::auth argus::contracts::sync argus::clients::sync)
target_link_libraries(gateway-core PRIVATE argus::contracts::gateway)

target_compile_options(gateway-core PRIVATE -Wall -Wextra)

add_executable(argus-gateway ${GATEWAY_SRC_ROOT}/main.cc)

target_link_libraries(argus-gateway PRIVATE
    $<LINK_LIBRARY:WHOLE_ARCHIVE,gateway-core>)

target_compile_options(argus-gateway PRIVATE -Wall -Wextra)

argus_runtime_rpath(argus-gateway)

add_executable(gateway-test ${CMAKE_CURRENT_SOURCE_DIR}/tests/gateway-test.cc)

target_include_directories(gateway-test PRIVATE ${GATEWAY_SRC_ROOT})

target_link_libraries(gateway-test PRIVATE gateway-core doctest::doctest
                                             argus::contracts::auth
                                             argus::lib::sqlite)

target_compile_options(gateway-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(gateway-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set_target_properties(gateway-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME gateway-test COMMAND gateway-test)

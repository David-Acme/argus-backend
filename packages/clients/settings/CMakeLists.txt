cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
if(NOT COMMAND argus_settings_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/settings
                     ${CMAKE_BINARY_DIR}/contracts/settings)
endif()
argus_settings_rpc_contract()

if(NOT TARGET argus::clients::settings)
    argus_clients(NAME settings
        SOURCES
            src/settings/settings-client.cc
        INCLUDES
            src
        DEPENDS
            argus::lib::config argus::lib::errors
            argus::contracts::settings)

    target_link_libraries(argus_clients_settings PRIVATE argus::contracts::settings-wire)
    target_compile_options(argus_clients_settings PRIVATE -Wall -Wextra)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(settings-client-test tests/unit/settings-client-test.cc)

set_target_properties(settings-client-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_link_libraries(settings-client-test PRIVATE argus::clients::settings
    argus::contracts::settings-wire doctest::doctest)

target_compile_options(settings-client-test PRIVATE -Wall -Wextra)

add_test(NAME settings-client-test COMMAND settings-client-test)

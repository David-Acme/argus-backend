cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME settings
    INCLUDES src
    SOURCES
        src/settings/settings-errors.hxx
    DEPENDS argus::lib::errors)

if(NOT COMMAND argus_response_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response
                     ${CMAKE_BINARY_DIR}/contracts/response)
endif()
function(argus_settings_rpc_contract)
    argus_response_rpc_contract()
    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../lib/config
                         ${CMAKE_BINARY_DIR}/lib/config)
    endif()
    if(NOT TARGET argus_client_grpc_base)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../lib/grpc
                         ${CMAKE_CURRENT_BINARY_DIR}/grpc)
    endif()
    if(NOT TARGET argus::contracts::settings-wire)
        argus_client_module(NAME settings-wire GROUP contracts
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/settings.proto
            SOURCES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/settings/settings-rpc.cc
                    ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/settings/settings-rpc.hxx
            INCLUDES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src
            DEPENDS argus::contracts::response-wire
                    argus::contracts::settings
                    argus::lib::config)
        target_compile_options(argus_contracts_settings-wire PRIVATE -Wall -Wextra)
    endif()
endfunction()

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
argus_settings_rpc_contract()

enable_testing()
find_package(doctest REQUIRED)

add_executable(settings-contract-rpc-test tests/unit/settings-contract-rpc-test.cc)
target_link_libraries(settings-contract-rpc-test PRIVATE
    argus::contracts::settings-wire doctest::doctest)
target_compile_options(settings-contract-rpc-test PRIVATE -Wall -Wextra)
add_test(NAME settings-contract-rpc-test COMMAND settings-contract-rpc-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

function(argus_response_rpc_contract)
    if(NOT TARGET argus_client_grpc_base)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../lib/grpc
                         ${CMAKE_CURRENT_BINARY_DIR}/grpc)
    endif()
    if(NOT TARGET argus::contracts::response-wire)
        argus_client_module(NAME response-wire GROUP contracts
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/response.proto
            SOURCES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/response/response-rpc.cc
                    ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/response/response-rpc.hxx
            INCLUDES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src
            DEPENDS argus::lib::errors)
        target_compile_options(argus_contracts_response-wire PRIVATE -Wall -Wextra)
    endif()
endfunction()

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
argus_response_rpc_contract()

enable_testing()
find_package(doctest REQUIRED)

add_executable(response-contract-rpc-test
    tests/unit/response-contract-rpc-test.cc)
target_link_libraries(response-contract-rpc-test PRIVATE
    argus::contracts::response-wire doctest::doctest)
target_compile_options(response-contract-rpc-test PRIVATE -Wall -Wextra)
add_test(NAME response-contract-rpc-test COMMAND response-contract-rpc-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

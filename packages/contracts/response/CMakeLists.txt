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
            SOURCES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/response-rpc.cc
            INCLUDES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            DEPENDS argus::lib::errors)
        target_compile_options(argus_contracts_response-wire PRIVATE -Wall -Wextra)
    endif()
endfunction()

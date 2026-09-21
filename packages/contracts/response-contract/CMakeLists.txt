cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

function(argus_response_rpc_contract)
    if(NOT TARGET argus_client_grpc_base)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../grpc
                         ${CMAKE_CURRENT_BINARY_DIR}/grpc)
    endif()
    if(NOT TARGET argus::client-response-wire)
        argus_client_module(NAME response-wire
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/response.proto
            SOURCES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/response-rpc.cc
            INCLUDES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            DEPENDS argus::errors)
        target_compile_options(argus_client_response-wire PRIVATE -Wall -Wextra)
    endif()
endfunction()

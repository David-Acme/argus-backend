cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

function(argus_response_rpc_contract)
    if(NOT TARGET argus::sdk-response-wire)
        argus_sdk_module(NAME response-wire
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/response.proto
            SOURCES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/response-rpc.cc
            INCLUDES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            DEPENDS pkg::response)
        target_compile_options(argus_sdk_response-wire PRIVATE -Wall -Wextra)
    endif()
endfunction()

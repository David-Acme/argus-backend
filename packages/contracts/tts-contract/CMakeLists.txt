cmake_minimum_required(VERSION 3.25)

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

add_library(contract-tts INTERFACE)
add_library(contract::tts ALIAS contract-tts)
target_include_directories(contract-tts INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(contract-tts INTERFACE pkg::response)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)
if(NOT COMMAND argus_response_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/response-contract)
endif()
function(argus_tts_rpc_contract)
    argus_response_rpc_contract()
    if(NOT TARGET argus_client_grpc_base)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../grpc
                         ${CMAKE_CURRENT_BINARY_DIR}/grpc)
    endif()
    if(NOT TARGET argus::client-tts-wire)
        argus_client_module(NAME tts-wire
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tts.proto
            DEPENDS argus::client-response-wire)
    endif()
endfunction()

cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

function(argus_mcp_rpc_contract)
    if(NOT TARGET argus::lib::mcp)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../lib/mcp
                         ${CMAKE_BINARY_DIR}/mcp)
    endif()
    if(NOT TARGET argus_client_grpc_base)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../lib/grpc
                         ${CMAKE_BINARY_DIR}/grpc)
    endif()
    if(NOT TARGET argus::contracts::mcp-wire)
        argus_client_module(NAME mcp-wire GROUP contracts
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/mcp.proto
            SOURCES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/mcp/mcp-rpc.cc
                    ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src/mcp/mcp-rpc.hxx
            INCLUDES ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/src
            DEPENDS argus::lib::mcp)
        target_compile_options(argus_contracts_mcp-wire PRIVATE -Wall -Wextra)
    endif()
endfunction()

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
argus_mcp_rpc_contract()

enable_testing()
find_package(doctest REQUIRED)

add_executable(mcp-rpc-test tests/unit/mcp-rpc-test.cc)
target_link_libraries(mcp-rpc-test PRIVATE argus::contracts::mcp-wire doctest::doctest)
target_compile_options(mcp-rpc-test PRIVATE -Wall -Wextra)
add_test(NAME mcp-rpc-test COMMAND mcp-rpc-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

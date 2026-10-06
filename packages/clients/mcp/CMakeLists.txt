cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL TRUE)
argus_contracts_substrate()
if(NOT ARGUS_SYSTEM_PROTOBUF AND NOT TARGET gRPC::grpc++)
    find_package(gRPC CONFIG REQUIRED)
endif()
if(NOT COMMAND argus_mcp_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../contracts/mcp
                     ${CMAKE_BINARY_DIR}/contracts/mcp)
endif()
argus_mcp_rpc_contract()

if(NOT TARGET argus::clients::mcp)
    argus_clients(NAME mcp
        SOURCES
            src/mcp/grpc-tool-transport.cc
            src/mcp/grpc-tool-transport.hxx
        INCLUDES
            src
        DEPENDS
            argus::lib::mcp)

    target_link_libraries(argus_clients_mcp PRIVATE argus::contracts::mcp-wire)
    target_compile_options(argus_clients_mcp PRIVATE -Wall -Wextra)
endif()

enable_testing()
find_package(doctest REQUIRED)

add_executable(grpc-tool-transport-test tests/unit/grpc-tool-transport-test.cc)
target_link_libraries(grpc-tool-transport-test PRIVATE argus::clients::mcp
    argus::contracts::mcp-wire doctest::doctest)
target_compile_options(grpc-tool-transport-test PRIVATE -Wall -Wextra)
add_test(NAME grpc-tool-transport-test COMMAND grpc-tool-transport-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

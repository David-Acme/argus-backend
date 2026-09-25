cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME vlm
    INCLUDES src
    SOURCES
        src/vlm/vlm-errors.hxx
    DEPENDS argus::lib::errors)

if(NOT COMMAND argus_response_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response
                     ${CMAKE_BINARY_DIR}/contracts/response)
endif()
function(argus_vlm_rpc_contract)
    argus_response_rpc_contract()
    if(NOT TARGET argus_client_grpc_base)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../lib/grpc
                         ${CMAKE_CURRENT_BINARY_DIR}/grpc)
    endif()
    if(NOT TARGET argus::contracts::vlm-wire)
        argus_client_module(NAME vlm-wire GROUP contracts
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/vlm.proto
            DEPENDS argus::contracts::response-wire)
    endif()
endfunction()

enable_testing()
find_package(doctest REQUIRED)

add_executable(vlm-contract-catalog-test
    tests/unit/vlm-contract-catalog-test.cc)
target_link_libraries(vlm-contract-catalog-test PRIVATE
    argus::contracts::vlm doctest::doctest)
target_compile_options(vlm-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME vlm-contract-catalog-test COMMAND vlm-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

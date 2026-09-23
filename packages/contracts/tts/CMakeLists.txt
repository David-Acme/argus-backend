cmake_minimum_required(VERSION 3.25)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

argus_contracts(NAME tts
    INCLUDES src
    SOURCES
        src/tts/tts-errors.hxx
    DEPENDS argus::lib::errors)

if(NOT COMMAND argus_response_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../response
                     ${CMAKE_BINARY_DIR}/contracts/response)
endif()
function(argus_tts_rpc_contract)
    argus_response_rpc_contract()
    if(NOT TARGET argus_client_grpc_base)
        add_subdirectory(${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../../lib/grpc
                         ${CMAKE_CURRENT_BINARY_DIR}/grpc)
    endif()
    if(NOT TARGET argus::contracts::tts-wire)
        argus_client_module(NAME tts-wire GROUP contracts
            PROTO_ROOT ${CMAKE_CURRENT_FUNCTION_LIST_DIR}
            PROTO ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/tts.proto
            DEPENDS argus::contracts::response-wire)
    endif()
endfunction()

enable_testing()
find_package(doctest REQUIRED)

add_executable(tts-contract-catalog-test
    tests/unit/tts-contract-catalog-test.cc)
target_link_libraries(tts-contract-catalog-test PRIVATE
    argus::contracts::tts doctest::doctest)
target_compile_options(tts-contract-catalog-test PRIVATE -Wall -Wextra)
add_test(NAME tts-contract-catalog-test COMMAND tts-contract-catalog-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

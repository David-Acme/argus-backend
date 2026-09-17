cmake_minimum_required(VERSION 3.25)

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

add_library(contract-gateway INTERFACE)
add_library(contract::gateway ALIAS contract-gateway)
target_include_directories(contract-gateway INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(contract-gateway INTERFACE pkg::response)

cmake_minimum_required(VERSION 3.25)

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

add_library(contract-sync INTERFACE)
add_library(contract::sync ALIAS contract-sync)
target_include_directories(contract-sync INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(contract-sync INTERFACE pkg::response)

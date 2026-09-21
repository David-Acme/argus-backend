cmake_minimum_required(VERSION 3.25)

if(NOT TARGET argus::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

add_library(contract-gateway INTERFACE)
add_library(contract::gateway ALIAS contract-gateway)
target_include_directories(contract-gateway INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(contract-gateway INTERFACE argus::errors)

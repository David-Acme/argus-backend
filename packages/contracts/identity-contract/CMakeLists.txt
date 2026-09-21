cmake_minimum_required(VERSION 3.25)

if(NOT TARGET argus::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

add_library(contract-identity INTERFACE)
add_library(contract::identity ALIAS contract-identity)
target_include_directories(contract-identity INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(contract-identity INTERFACE argus::errors)

cmake_minimum_required(VERSION 3.25)

if(NOT TARGET pkg::response)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../response
                     ${CMAKE_CURRENT_BINARY_DIR}/response)
endif()

if(NOT TARGET argus::access)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../access
                     ${CMAKE_CURRENT_BINARY_DIR}/access)
endif()

if(NOT TARGET argus::json)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../json
                     ${CMAKE_CURRENT_BINARY_DIR}/json)
endif()

if(NOT TARGET Drogon::Drogon)
    find_package(Drogon CONFIG REQUIRED)
endif()

add_library(contract-sync INTERFACE)
add_library(contract::sync ALIAS contract-sync)
target_include_directories(contract-sync INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_sources(contract-sync INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/sync-operation.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/sync-filter.hxx
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shared/contracts/syncable.hxx)
target_link_libraries(contract-sync INTERFACE pkg::response argus::access
    argus::json Drogon::Drogon)

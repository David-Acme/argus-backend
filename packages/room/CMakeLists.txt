# argus_room: the WebSocket room registry behind the sync socket and the
# per-service emitters. Consumers link argus::room.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET argus::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../auth
                     ${CMAKE_CURRENT_BINARY_DIR}/auth)
endif()

if(NOT TARGET contract::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/auth-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/auth-contract)
endif()

if(NOT TARGET contract::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/sync-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/sync-contract)
endif()

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME room
    SOURCES
        src/shared/services/room/room-manager.cc
    INCLUDES src
    DEPENDS
        argus::auth
        contract::auth
        contract::sync)

target_link_libraries(argus_room PUBLIC Drogon::Drogon)

target_compile_options(argus_room PRIVATE -Wall -Wextra)

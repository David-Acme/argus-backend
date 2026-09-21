# argus_room: the WebSocket room registry behind the sync socket and the
# per-service emitters. Consumers link argus::room.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET argus::lib::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/auth
                     ${CMAKE_CURRENT_BINARY_DIR}/auth)
endif()

if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME room
    SOURCES
        src/shared/services/room/room-manager.cc
    INCLUDES src
    DEPENDS
        argus::lib::auth
        argus::contracts::auth
        argus::contracts::sync)

target_link_libraries(argus_room PUBLIC Drogon::Drogon)

target_compile_options(argus_room PRIVATE -Wall -Wextra)

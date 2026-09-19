# argus_room: the WebSocket room registry behind the sync socket and the
# per-service emitters. Consumers link argus::room.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(NOT TARGET argus::access)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../access
                     ${CMAKE_CURRENT_BINARY_DIR}/access)
endif()

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME room
    SOURCES
        src/shared/services/room/room-manager.cc
    INCLUDES src)

target_link_libraries(argus_room PUBLIC argus::access Drogon::Drogon)

target_compile_options(argus_room PRIVATE -Wall -Wextra)

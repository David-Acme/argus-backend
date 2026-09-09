# argus_room: the WebSocket room registry behind the sync socket and the
# per-service emitters. Consumers link argus::room.

find_package(Drogon CONFIG REQUIRED)

argus_module(NAME room
    SOURCES
        src/shared/services/room/room-manager.cc
    INCLUDES src)

target_link_libraries(argus_room
    PUBLIC
        argus_common
        Drogon::Drogon)

target_compile_options(argus_room PRIVATE -Wall -Wextra)

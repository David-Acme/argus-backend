# argus_socket: the per-service WebSocket emitter (SocketService) and the
# NATS identity-change sink. Consumers link argus::socket.

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_module(NAME socket
    SOURCES
        ../shared/services/socket/socket-service.cc
        ../shared/services/socket/nats-identity-change-sink.cc
    INCLUDES ..)

target_link_libraries(argus_socket
    PUBLIC
        argus::room
        argus_common
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_socket PRIVATE -Wall -Wextra)

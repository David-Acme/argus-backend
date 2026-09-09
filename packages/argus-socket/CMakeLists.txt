# argus_socket: the per-service WebSocket emitter (SocketService) and the
# NATS identity-change sink. Consumers link argus::socket.

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_module(NAME socket
    SOURCES
        src/shared/services/socket/socket-service.cc
        src/shared/services/socket/nats-identity-change-sink.cc
    INCLUDES src)

target_link_libraries(argus_socket
    PUBLIC
        argus::room
        argus_common
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_socket PRIVATE -Wall -Wextra)

# The sync-change payload shape the socket module owns.
if(ARGUS_ROOT_PROJECT)
    find_package(doctest REQUIRED)

    add_executable(sync-change-test tests/unit/sync-change-test.cc)
    target_link_libraries(sync-change-test PRIVATE argus::socket argus_common
        doctest::doctest)
    target_compile_options(sync-change-test PRIVATE -Wall -Wextra)
    add_test(NAME sync-change-test COMMAND sync-change-test)
endif()

argus_module(NAME camera-event-stream
    SOURCES
        event-stream.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::contracts::sync
        argus::lib::nats
        Drogon::Drogon)

target_compile_options(argus_camera-event-stream PRIVATE -Wall -Wextra)

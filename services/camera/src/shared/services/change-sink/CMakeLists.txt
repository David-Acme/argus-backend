argus_module(NAME camera-change-sink
    SOURCES
        nats-camera-change-sink.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::camera-event-stream
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::errors
        argus::lib::nats
        argus::lib::outbox
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_camera-change-sink PRIVATE -Wall -Wextra)

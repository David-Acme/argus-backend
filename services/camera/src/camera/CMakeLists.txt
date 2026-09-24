argus_module(NAME camera-core
    SOURCES
        camera-config.cc
        nats-camera-change-sink.cc
    INCLUDES
        ..
    DEPENDS
        argus::camera-change-outbox
        argus::camera-event-stream
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::config
        argus::lib::errors
        argus::lib::nats
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_camera-core PRIVATE -Wall -Wextra)

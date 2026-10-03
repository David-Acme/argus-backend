argus_module(NAME camera-monitor
    SOURCES
        camera-health-monitor.cc
        camera-presence.cc
        nats-health-event-sink.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-config
        argus::camera-driver
        argus::camera-operator
        argus::camera-repositories
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        opencv::opencv
        Drogon::Drogon)

target_compile_options(argus_camera-monitor PRIVATE -Wall -Wextra)

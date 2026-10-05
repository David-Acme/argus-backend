argus_module(NAME camera-media
    SOURCES
        camera-media-service.cc
        camera-media-socket.cc
        media-access-check.cc
        media-session-registry.cc
        session-revocation-consumer.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-repositories
        argus::camera-stream
        argus::camera-talk
        argus::clients::auth
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::nats
        argus::lib::runtime
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_camera-media PRIVATE -Wall -Wextra)

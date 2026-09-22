argus_module(NAME sync-transport
    SOURCES
        infra/camera-sync-source.cc
        infra/notification-sync-source.cc
        infra/productivity-sync-source.cc
        infra/sync-socket-registrar.cc
        infra/voice-grpc-relay.cc
    INCLUDES ../..
    DEPENDS
        argus_sync
        argus::clients::camera
        argus::clients::notification
        argus::clients::productivity
        argus::clients::voice
        argus::lib::config
        argus::lib::runtime
        argus::contracts::sync
        argus::contracts::voice
        Drogon::Drogon)

target_compile_options(argus_sync-transport PRIVATE -Wall -Wextra)

argus_module(NAME sync-transport
    SOURCES
        controllers/sync-socket.cc
        services/sync-service.cc
        services/synchronized-service.cc
        repositories/event/event-repository.cc
        schemas/event/event-schema.cc
        schemas/person-event/person-event-schema.cc
        infra/camera-sync-gateway.cc
        infra/notification-sync-gateway.cc
        infra/productivity-sync-gateway.cc
        infra/sync-socket-registrar.cc
        infra/voice-grpc-relay.cc
    INCLUDES ../..
    DEPENDS
        argus::sync-repositories
        argus::sync-services
        argus::identity
        argus::clients::camera
        argus::clients::notification
        argus::clients::productivity
        argus::clients::voice
        argus::contracts::auth
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_sync-transport PRIVATE -Wall -Wextra)

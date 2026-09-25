argus_module(NAME camera-operator
    SOURCES
        camera-operator-service.cc
        event-intelligence.cc
        go2rtc-frame-source.cc
        identity-known-person-matcher.cc
        nats-object-event-sink.cc
        object-event.cc
        zone-provider.cc
        zone-source.cc
        repositories/object-event-outbox/object-event-outbox-repository.cc
        services/evidence/evidence-uploader.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-config
        argus::camera-event-stream
        argus::camera-objects
        argus::camera-repositories
        argus::camera-stream
        argus::clients::identity
        argus::contracts::camera
        argus::contracts::sync
        argus::lib::config
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::storage
        argus::lib::text
        opencv::opencv
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_camera-operator PRIVATE -Wall -Wextra)

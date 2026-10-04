argus_module(NAME notification-call
    SOURCES
        controllers/call-preference-controller.cc
        dtos/update-call-preference-dto.cc
        infra/identity-call-directory.cc
        infra/notification-call-sink.cc
        infra/sync-call-signal.cc
        infra/voice-call-announcer.cc
        repositories/arrival-seen/arrival-seen-repository.cc
        repositories/call/call-repository.cc
        repositories/call-preference/call-preference-repository.cc
        repositories/scheduled-call/scheduled-call-repository.cc
        schemas/call/call-schema.cc
        schemas/call-preference/call-preference-schema.cc
        services/call-copy.cc
        services/call-engine.cc
        services/call-feed.cc
        services/call-policy.cc
        services/call-preference-service.cc
        services/call-trigger-classifier.cc
    INCLUDES ../..
    DEPENDS
        argus::notification-shared
        argus::notification-config
        argus::clients::identity
        argus::clients::sync
        argus::clients::voice
        argus::contracts::sync
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_notification-call PRIVATE -Wall -Wextra)

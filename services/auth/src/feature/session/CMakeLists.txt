argus_module(NAME auth-session
    SOURCES
        schemas/refresh-token/refresh-token-schema.cc
        repositories/change-outbox/change-outbox-repository.cc
        repositories/refresh-token/refresh-token-repository.cc
        services/auth-action-sink.cc
        services/session-context-cache.cc
        services/session-events.cc
        services/session-revocation.cc
        services/session-service.cc
        services/identity-change-consumer.cc
        services/presence-signal.cc
        infra/nats-presence-signal-sink.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::auth
        argus::lib::auth
        argus::lib::errors
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::contracts::sync
        argus::clients::identity
        Drogon::Drogon)

target_compile_options(argus_auth-session PRIVATE -Wall -Wextra)

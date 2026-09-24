argus_module(NAME auth-session
    SOURCES
        schemas/refresh-token/refresh-token-schema.cc
        repositories/refresh-token/refresh-token-repository.cc
        services/session-context-cache.cc
        services/session-service.cc
        services/identity-change-consumer.cc
    INCLUDES ../..
    DEPENDS
        argus::lib::auth
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::contracts::sync
        argus::clients::identity
        Drogon::Drogon)

target_compile_options(argus_auth-session PRIVATE -Wall -Wextra)

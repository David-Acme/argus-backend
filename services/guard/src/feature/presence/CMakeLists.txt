argus_module(NAME guard-presence
    SOURCES
        services/presence-engine.cc
        services/presence-service.cc
        infra/identity-presence-directory.cc
        infra/nats-presence-publisher.cc
        controllers/presence-controller.cc
        dtos/response-presence-dto.cc
    INCLUDES
        ../..
    DEPENDS
        argus::guard-config
        argus::guard-presence-repository
        argus::clients::identity
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::http
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_guard-presence PRIVATE -Wall -Wextra)

argus_module(NAME sync-heartbeat
    SOURCES
        controllers/heartbeat-controller.cc
        dtos/response-heartbeat-dto.cc
        services/heartbeat-policy.cc
        services/presence-board.cc
        services/heartbeat-service.cc
        services/heartbeat-feed.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::nats
        argus::lib::runtime
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_sync-heartbeat PRIVATE -Wall -Wextra)

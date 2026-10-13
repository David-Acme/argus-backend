argus_module(NAME sync-remote
    SOURCES
        services/remote-link-gate.cc
        services/remote-link-feed.cc
    INCLUDES ../..
    DEPENDS
        argus::sync-services
        argus::contracts::routes
        argus::lib::nats
        Drogon::Drogon)

target_compile_options(argus_sync-remote PRIVATE -Wall -Wextra)

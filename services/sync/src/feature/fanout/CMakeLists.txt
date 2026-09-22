argus_module(NAME sync-fanout
    SOURCES
        services/audit-fan-out.cc
        services/audit-log-service.cc
        services/notification-delivery-consumer.cc
        services/sync-fan-out.cc
        services/user-audit-log-service.cc
        repositories/delivery-inbox/delivery-inbox-repository.cc
    INCLUDES ../..
    DEPENDS
        argus::sync-repositories
        argus::sync-services
        argus::lib::nats
        argus::lib::sqlite
        argus::lib::text
        argus::contracts::auth
        argus::contracts::sync
        argus::contracts::notification
        Drogon::Drogon)

target_compile_options(argus_sync-fanout PRIVATE -Wall -Wextra)

argus_module(NAME sync-fanout
    SOURCES
        services/audit-fan-out.cc
        services/notification-delivery-consumer.cc
        services/sync-fan-out.cc
        repositories/delivery-inbox/delivery-inbox-repository.cc
    INCLUDES ../..
    DEPENDS
        argus_sync
        argus::lib::nats
        argus::lib::sqlite
        argus::lib::text
        argus::contracts::sync
        argus::contracts::notification
        Drogon::Drogon)

target_compile_options(argus_sync-fanout PRIVATE -Wall -Wextra)

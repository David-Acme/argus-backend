argus_module(NAME notification-delivery-sink
    SOURCES
        nats-notification-delivery-sink.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::contracts::notification
        argus::contracts::sync
        argus::lib::nats
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_notification-delivery-sink PRIVATE -Wall -Wextra)

argus_module(NAME notification-change-sink
    SOURCES
        nats-notification-change-sink.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::contracts::notification
        argus::contracts::sync
        argus::lib::errors
        argus::lib::nats
        argus::lib::text
        argus::notification-change-outbox
        Drogon::Drogon
        ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(argus_notification-change-sink PRIVATE -Wall -Wextra)

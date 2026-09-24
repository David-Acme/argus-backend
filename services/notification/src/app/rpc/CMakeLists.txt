argus_module(NAME notification-rpc
    SOURCES
        notification-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        notification-core
        argus::clients::notification
        argus::lib::config
        argus::lib::nats
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_notification-rpc PRIVATE -Wall -Wextra)

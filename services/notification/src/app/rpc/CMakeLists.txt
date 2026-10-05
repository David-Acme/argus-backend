argus_module(NAME notification-rpc
    SOURCES
        call-rpc-service.cc
        notification-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        argus::notification-shared
        argus::notification-config
        argus::notification-call
        argus::clients::notification
        argus::lib::config
        argus::lib::nats
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_notification-rpc PRIVATE -Wall -Wextra)

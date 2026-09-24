argus_module(NAME productivity-core
    SOURCES
        productivity-config.cc
        nats-productivity-change-sink.cc
    INCLUDES
        ..
    DEPENDS
        argus::productivity-change-outbox
        argus::contracts::productivity
        argus::contracts::sync
        argus::lib::config
        argus::lib::errors
        argus::lib::nats
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_productivity-core PRIVATE -Wall -Wextra)

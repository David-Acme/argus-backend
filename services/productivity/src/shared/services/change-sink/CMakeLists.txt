argus_module(NAME productivity-change-sink
    SOURCES
        nats-productivity-change-sink.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::productivity-change-outbox
        argus::contracts::productivity
        argus::contracts::sync
        argus::lib::errors
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_productivity-change-sink PRIVATE -Wall -Wextra)

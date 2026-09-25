argus_module(NAME guard-config
    SOURCES
        guard-config.cc
    INCLUDES
        ..
    DEPENDS
        argus::lib::config
        argus::lib::http
        argus::lib::nats
        Drogon::Drogon)

target_compile_options(argus_guard-config PRIVATE -Wall -Wextra)

argus_module(NAME guard-rpc
    SOURCES
        presence-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        argus::guard-presence
        argus::clients::guard
        Drogon::Drogon)

target_compile_options(argus_guard-rpc PRIVATE -Wall -Wextra)

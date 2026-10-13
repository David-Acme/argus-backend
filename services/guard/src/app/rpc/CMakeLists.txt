argus_module(NAME guard-rpc
    SOURCES
        duress-rpc-service.cc
        presence-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        argus::guard-presence
        argus::guard-safety
        argus::clients::guard
        argus::lib::auth
        Drogon::Drogon)

target_compile_options(argus_guard-rpc PRIVATE -Wall -Wextra)

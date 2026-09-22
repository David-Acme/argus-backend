argus_module(NAME sync-control-rpc
    SOURCES
        sync-control-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        argus_sync
        argus::sync-fanout
        argus::clients::sync
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::text)

target_compile_options(argus_sync-control-rpc PRIVATE -Wall -Wextra)

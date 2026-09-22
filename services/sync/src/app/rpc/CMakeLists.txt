argus_module(NAME sync-control-rpc
    SOURCES
        sync-control-rpc-service.cc
    INCLUDES ../..
    DEPENDS
        argus::sync-repositories
        argus::sync-services
        argus::sync-fanout
        argus::clients::sync
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::grpc-health
        argus::lib::text)

target_compile_options(argus_sync-control-rpc PRIVATE -Wall -Wextra)

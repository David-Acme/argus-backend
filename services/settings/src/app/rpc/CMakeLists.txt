argus_module(NAME settings-rpc
    SOURCES
        modules-rpc-service.cc
        modules-rpc-server.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::settings
        argus::contracts::settings-wire)

target_compile_options(argus_settings-rpc PRIVATE -Wall -Wextra)

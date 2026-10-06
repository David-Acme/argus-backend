argus_module(NAME settings-mcp
    SOURCES
        infra/engine-module-desk.cc
        services/module-tools.cc
    INCLUDES
        ../..
    DEPENDS
        argus::contracts::mcp-wire
        argus::lib::auth
        argus::lib::errors
        argus::settings-modules
        argus::lib::mcp
        Drogon::Drogon)

target_compile_options(argus_settings-mcp PRIVATE -Wall -Wextra)

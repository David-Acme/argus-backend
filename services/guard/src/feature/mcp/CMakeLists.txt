argus_module(NAME guard-mcp
    SOURCES
        services/guard-tools.cc
    INCLUDES
        ../..
    DEPENDS
        argus::contracts::mcp-wire
        argus::lib::auth
        argus::lib::mcp
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_guard-mcp PRIVATE -Wall -Wextra)

argus_module(NAME camera-mcp
    SOURCES
        services/camera-tools.cc
    INCLUDES
        ../..
    DEPENDS
        argus::camera-repositories
        argus::contracts::mcp-wire
        argus::lib::auth
        argus::lib::mcp
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_camera-mcp PRIVATE -Wall -Wextra)

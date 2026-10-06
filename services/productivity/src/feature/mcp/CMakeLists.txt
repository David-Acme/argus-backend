argus_module(NAME productivity-mcp
    SOURCES
        services/calendar-tools.cc
        services/planner-tools.cc
        services/productivity-tools.cc
        services/tool-support.cc
    INCLUDES
        ../..
    DEPENDS
        argus::productivity-calendar-event
        argus::productivity-project
        argus::productivity-project-task
        argus::productivity-repositories
        argus::contracts::mcp-wire
        argus::lib::auth
        argus::lib::errors
        argus::lib::mcp
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_productivity-mcp PRIVATE -Wall -Wextra)

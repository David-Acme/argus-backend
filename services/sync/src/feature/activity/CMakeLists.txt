argus_module(NAME sync-activity
    SOURCES
        controllers/activity-controller.cc
        dtos/list-activity-dto.cc
        dtos/response-activity-dto.cc
        services/activity-feature-service.cc
    INCLUDES ../..
    DEPENDS
        argus::sync-repositories
        argus::contracts::sync
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_sync-activity PRIVATE -Wall -Wextra)

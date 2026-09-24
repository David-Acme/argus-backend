argus_module(NAME productivity-project-task
    SOURCES
        controllers/project-task-controller.cc
        dtos/create-project-task-dto.cc
        dtos/update-project-task-dto.cc
        services/project-task-feature-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::productivity-repositories
        argus::contracts::productivity
        argus::contracts::sync
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_productivity-project-task PRIVATE -Wall -Wextra)

argus_module(NAME productivity-project
    SOURCES
        controllers/project-controller.cc
        dtos/create-project-dto.cc
        dtos/update-project-dto.cc
        services/project-feature-service.cc
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

target_compile_options(argus_productivity-project PRIVATE -Wall -Wextra)

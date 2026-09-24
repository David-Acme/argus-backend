argus_module(NAME productivity-project-member
    SOURCES
        controllers/project-member-controller.cc
        dtos/create-project-member-dto.cc
        dtos/update-project-member-dto.cc
        services/project-member-feature-service.cc
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

target_compile_options(argus_productivity-project-member PRIVATE -Wall -Wextra)

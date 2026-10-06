argus_module(NAME productivity-reminder
    SOURCES
        controllers/reminder-controller.cc
        dtos/create-reminder-dto.cc
        dtos/update-reminder-dto.cc
        reminder-rpc-service.cc
        services/reminder-feature-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::productivity-dtos
        argus::productivity-repositories
        argus::clients::productivity
        argus::contracts::productivity
        argus::contracts::sync
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_productivity-reminder PRIVATE -Wall -Wextra)

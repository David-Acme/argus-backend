argus_module(NAME productivity-calendar-event
    SOURCES
        controllers/calendar-event-controller.cc
        dtos/create-calendar-event-dto.cc
        dtos/update-calendar-event-dto.cc
        services/calendar-event-feature-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::productivity-dtos
        argus::productivity-repositories
        argus::contracts::productivity
        argus::contracts::sync
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_productivity-calendar-event PRIVATE -Wall -Wextra)

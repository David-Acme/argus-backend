argus_module(NAME productivity-calendar-event-share
    SOURCES
        controllers/calendar-event-share-controller.cc
        dtos/create-calendar-event-share-dto.cc
        dtos/update-calendar-event-share-dto.cc
        services/calendar-event-share-feature-service.cc
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

target_compile_options(argus_productivity-calendar-event-share PRIVATE -Wall -Wextra)

argus_module(NAME productivity-agenda
    SOURCES
        infra/notification-agenda-notifier.cc
        repositories/agenda-announcement/agenda-announcement-repository.cc
        services/agenda-announcer.cc
        services/agenda-sweeper.cc
    INCLUDES ../..
    DEPENDS
        argus::clients::notification
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_productivity-agenda PRIVATE -Wall -Wextra)

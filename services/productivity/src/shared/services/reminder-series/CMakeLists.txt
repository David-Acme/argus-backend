argus_module(NAME productivity-reminder-series
    SOURCES
        reminder-series.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::productivity-repositories
        argus::productivity-recurrence
        argus::contracts::sync
        Drogon::Drogon)

target_compile_options(argus_productivity-reminder-series PRIVATE -Wall -Wextra)

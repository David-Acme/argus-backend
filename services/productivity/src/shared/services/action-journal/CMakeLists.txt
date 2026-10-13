argus_module(NAME productivity-action-journal
    SOURCES
        action-journal.cc
    INCLUDES
        ../../..
    DEPENDS
        argus::productivity-repositories
        argus::contracts::productivity
        argus::contracts::sync
        Drogon::Drogon)

target_compile_options(argus_productivity-action-journal PRIVATE -Wall -Wextra)

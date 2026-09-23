argus_module(NAME sync-services
    SOURCES
        room/room-manager.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::auth
        Drogon::Drogon)

target_compile_options(argus_sync-services PRIVATE -Wall -Wextra)

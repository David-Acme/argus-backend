argus_module(NAME sync-services
    SOURCES
        context/user-context.cc
        room/room-manager.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::auth
        argus::lib::runtime
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_sync-services PRIVATE -Wall -Wextra)

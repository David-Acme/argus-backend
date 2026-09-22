# The one service two features share: the room registry the transport's
# SyncService, the fan-out's SyncFanOut and main.cc's lifecycle object all hold.
# The two audit writers are not here — each has one reader, the fan-out's
# AuditFanOut, so they live in that feature (rule 23).
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

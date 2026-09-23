argus_module(NAME sync-repositories
    SOURCES
        audit-log/audit-log-repository.cc
        user-audit-log/user-audit-log-repository.cc
        user-action-log/user-action-log-repository.cc
        ../schemas/audit-log/audit-log-schema.cc
        ../schemas/user-audit-log/user-audit-log-schema.cc
        ../schemas/user-action-log/user-action-log-schema.cc
    INCLUDES ../..
    DEPENDS
        argus::contracts::sync
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_sync-repositories PRIVATE -Wall -Wextra)

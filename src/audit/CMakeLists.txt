# argus_audit: the cross-domain audit trail (audit_log, user_audit_log,
# user_action_log) and the SyncAudit entry every feature writes through.
# Consumers link argus::audit.

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_module(NAME audit
    SOURCES
        ../shared/repositories/audit-log/audit-log-repository.cc
        ../shared/repositories/user-action-log/user-action-log-repository.cc
        ../shared/repositories/user-audit-log/user-audit-log-repository.cc
        ../shared/schemas/audit-log/audit-log-schema.cc
        ../shared/schemas/user-action-log/user-action-log-schema.cc
        ../shared/schemas/user-audit-log/user-audit-log-schema.cc
        ../shared/services/audit-log/audit-log-service.cc
        ../shared/services/sync-audit/sync-audit-service.cc
        ../shared/services/user-action-log/user-action-log-service.cc
        ../shared/services/user-audit-log/user-audit-log-service.cc
    INCLUDES ..)

# SocketService still compiles inside argus_identity until it peels off on
# its own; the mutual edge is declared so CMake repeats the archives.
target_link_libraries(argus_audit
    PUBLIC
        argus::sqlite
        argus_common
        argus_identity
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_audit PRIVATE -Wall -Wextra)

# argus_audit: the cross-domain audit trail (audit_log, user_audit_log,
# user_action_log), the SyncAudit entry every feature writes through, and
# the socket-backed user-change sink. Consumers link argus::audit.

find_package(Drogon CONFIG REQUIRED)
find_package(nlohmann_json REQUIRED)

argus_module(NAME audit
    SOURCES
        src/shared/repositories/audit-log/audit-log-repository.cc
        src/shared/repositories/user-action-log/user-action-log-repository.cc
        src/shared/repositories/user-audit-log/user-audit-log-repository.cc
        src/shared/schemas/audit-log/audit-log-schema.cc
        src/shared/schemas/user-action-log/user-action-log-schema.cc
        src/shared/schemas/user-audit-log/user-audit-log-schema.cc
        src/shared/services/audit-log/audit-log-service.cc
        src/shared/services/sync-audit/sync-audit-service.cc
        src/shared/services/user-action-log/user-action-log-service.cc
        src/shared/services/user-audit-log/user-audit-log-service.cc
        src/shared/services/socket/socket-user-change-sink.cc
    INCLUDES src)

target_link_libraries(argus_audit
    PUBLIC
        argus::socket
        argus::sqlite
        argus_common
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_audit PRIVATE -Wall -Wextra)

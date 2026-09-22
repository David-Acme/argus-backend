# argus_audit: the cross-domain audit trail (audit_log, user_audit_log,
# user_action_log), the SyncAudit entry every feature writes through, and
# the socket-backed user-change sink. Consumers link argus::audit.

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# The sibling packages this module links, declared here rather than by the
# consumer: the guards keep the root tree and every other consumer in charge.
if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
endif()

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/text
                     ${CMAKE_CURRENT_BINARY_DIR}/text)
endif()

if(NOT TARGET argus::lib::errors)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/errors
                     ${CMAKE_CURRENT_BINARY_DIR}/errors)
endif()

if(NOT TARGET argus::lib::sqlite)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/sqlite
                     ${CMAKE_CURRENT_BINARY_DIR}/sqlite)
endif()

if(NOT TARGET argus::socket)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../socket
                     ${CMAKE_CURRENT_BINARY_DIR}/socket)
endif()

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
    INCLUDES src
    DEPENDS
        argus::contracts::sync)

target_link_libraries(argus_audit PUBLIC
    argus::socket
    argus::lib::sqlite
    argus::lib::text
    argus::lib::errors
    Drogon::Drogon
    nlohmann_json::nlohmann_json)

target_compile_options(argus_audit PRIVATE -Wall -Wextra)

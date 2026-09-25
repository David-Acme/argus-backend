argus_module(NAME notification-shared
    SOURCES
        repositories/notification/notification-repository.cc
        schemas/notification/notification-schema.cc
        services/notification/notification-service.cc
    INCLUDES
        ..
    DEPENDS
        argus::contracts::notification
        argus::contracts::sync
        argus::lib::nats
        argus::lib::sqlite
        argus::lib::text
        argus::notification-change-outbox
        ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(argus_notification-shared PRIVATE -Wall -Wextra)

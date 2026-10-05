argus_module(NAME notification-shared
    SOURCES
        repositories/notification/notification-repository.cc
        schemas/notification/notification-schema.cc
        services/notification/delivery-page.cc
        services/notification/notification-service.cc
        services/notification/push-copy.cc
    INCLUDES
        ..
    DEPENDS
        argus::contracts::notification
        argus::contracts::sync
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        ${ARGUS_SQLITE_VEC_TARGET})

target_compile_options(argus_notification-shared PRIVATE -Wall -Wextra)

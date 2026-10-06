argus_module(NAME notification-module-request
    SOURCES
        services/module-request-copy.cc
        services/notification-module-request.cc
    DEPENDS
        argus::lib::runtime
        argus::lib::text
        argus::clients::identity
        argus::contracts::settings-wire
        argus::notification-shared
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_compile_options(argus_notification-module-request PRIVATE -Wall -Wextra)

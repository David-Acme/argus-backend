argus_module(NAME notification-camera-notification
    SOURCES
        repositories/camera-fallback-log/camera-fallback-log-repository.cc
        services/camera-notification-policy.cc
        services/camera-object-notifier.cc
    DEPENDS
        argus::lib::config
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::clients::identity
        notification-core
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../..)

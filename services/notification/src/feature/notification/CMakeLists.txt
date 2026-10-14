argus_module(NAME notification-feature
    SOURCES
        controllers/notification-controller.cc
        controllers/notification-token-controller.cc
        dtos/delivery-summary-dto.cc
        dtos/notification-ack-dto.cc
        dtos/notification-read-dto.cc
        dtos/register-notification-token-dto.cc
        dtos/unregister-notification-token-dto.cc
        services/notification-feature-service.cc
        services/notification-token-feature-service.cc
        services/notification-token/notification-token-service.cc
        services/push-outcome/push-outcome-service.cc
        services/token-revocation/token-revocation.cc
    INCLUDES ../..
    DEPENDS
        argus::notification-shared
        argus::contracts::push
        argus::lib::text
        argus::notification-config
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::nats
        argus::lib::text
        argus::contracts::notification
        argus::contracts::sync
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_notification-feature PRIVATE -Wall -Wextra)

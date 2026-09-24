argus_module(NAME notification-feature
    SOURCES
        controllers/notification-controller.cc
        controllers/notification-token-controller.cc
        dtos/delivery-summary-dto.cc
        dtos/notification-ack-dto.cc
        dtos/notification-read-dto.cc
        dtos/register-notification-token-dto.cc
        repositories/notification-token/notification-token-repository.cc
        schemas/notification-token/notification-token-schema.cc
        services/notification-feature-service.cc
        services/notification-token-feature-service.cc
        services/notification-token/notification-token-service.cc
    INCLUDES ../..
    DEPENDS
        notification-core
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon
        nlohmann_json::nlohmann_json)

target_compile_options(argus_notification-feature PRIVATE -Wall -Wextra)

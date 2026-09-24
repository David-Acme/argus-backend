argus_module(NAME identity-user
    SOURCES
        controllers/portrait-preview-controller.cc
        controllers/user-controller.cc
        dtos/response-portrait-preview-capability-dto.cc
        dtos/response-portrait-preview-image-dto.cc
        dtos/update-user-dto.cc
        services/nats-identity-change-sink.cc
        services/portrait-preview-service.cc
        services/user-feature-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::identity-change-outbox
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::nats
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::storage
        argus::lib::text
        argus::lib::validation
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        Drogon::Drogon
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_identity-user PRIVATE -Wall -Wextra)

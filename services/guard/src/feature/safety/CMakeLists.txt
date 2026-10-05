argus_module(NAME guard-safety
    SOURCES
        controllers/safety-controller.cc
        dtos/panic-dto.cc
        dtos/response-safety-dto.cc
        dtos/set-pin-dto.cc
        dtos/update-safety-dto.cc
        infra/guard-alert-sink.cc
        infra/notification-actor-notifier.cc
        repositories/safety-alert/safety-alert-repository.cc
        repositories/safety-setting/safety-setting-repository.cc
        repositories/user-pin/user-pin-repository.cc
        services/pin-attempts.cc
        services/pin-hash.cc
        services/safety-copy.cc
        services/safety-service.cc
    INCLUDES
        ../..
    DEPENDS
        argus::guard
        argus::clients::identity
        argus::clients::notification
        argus::contracts::auth
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::lib::validation
        Drogon::Drogon
        OpenSSL::Crypto)

target_compile_options(argus_guard-safety PRIVATE -Wall -Wextra)

argus_module(NAME auth-auth
    SOURCES
        controllers/auth-controller.cc
        dtos/create-device-login-dto.cc
        dtos/device-login-status-dto.cc
        dtos/login-dto.cc
        dtos/refresh-token-dto.cc
        dtos/register-dto.cc
        dtos/response-login-dto.cc
        dtos/response-refresh-token-dto.cc
        dtos/update-me-dto.cc
        infra/refresh-rate-gate.cc
        services/auth-feature-service.cc
    INCLUDES ../..
    DEPENDS
        argus::auth-config
        argus::auth-device
        argus::auth-session
        argus::clients::identity
        argus::contracts::auth
        argus::contracts::sync
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::validation
        Drogon::Drogon)

target_compile_options(argus_auth-auth PRIVATE -Wall -Wextra)

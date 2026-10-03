argus_module(NAME identity-pairing
    SOURCES
        controllers/pairing-controller.cc
        dtos/pairing-dto.cc
        dtos/response-pairing-dto.cc
        dtos/response-pairing-status-dto.cc
        services/pairing-feature-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-config
        argus::identity-shared
        argus::lib::auth
        argus::lib::cert
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::validation
        argus::contracts::auth
        argus::contracts::identity
        Drogon::Drogon)

target_compile_options(argus_identity-pairing PRIVATE -Wall -Wextra)

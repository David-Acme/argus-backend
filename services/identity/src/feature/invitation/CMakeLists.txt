argus_module(NAME identity-invitation
    SOURCES
        controllers/invitation-controller.cc
        dtos/create-invitation-dto.cc
        dtos/resolve-invitation-dto.cc
        dtos/response-invitation-dto.cc
        dtos/response-invitation-resolve-dto.cc
        services/invitation-feature-service.cc
        services/invitation-module-revocation.cc
        services/invitation-standing.cc
        services/role-copy.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-config
        argus::identity-shared
        argus::lib::auth
        argus::lib::cert
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::sqlite
        argus::lib::validation
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        Drogon::Drogon
        OpenSSL::SSL
        OpenSSL::Crypto)

target_compile_options(argus_identity-invitation PRIVATE -Wall -Wextra)

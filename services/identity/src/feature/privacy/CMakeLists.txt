argus_module(NAME identity-privacy
    SOURCES
        controllers/privacy-controller.cc
        dtos/response-privacy-dto.cc
        dtos/update-household-privacy-dto.cc
        dtos/update-privacy-dto.cc
        services/privacy-feature-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::identity-voiceprint
        argus::lib::auth
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::validation
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        Drogon::Drogon)

target_compile_options(argus_identity-privacy PRIVATE -Wall -Wextra)

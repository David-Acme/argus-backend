argus_module(NAME settings-gateway
    SOURCES
        controllers/settings-controller.cc
        dtos/response-apply-profile-dto.cc
        dtos/response-list-profiles-dto.cc
        dtos/response-list-settings-dto.cc
        dtos/response-owner-catalog-dto.cc
        dtos/response-update-settings-dto.cc
        dtos/setting-names.cc
        dtos/update-settings-dto.cc
        infra/hardware-facts.cc
        infra/profile-file.cc
        services/owner-visibility.cc
        services/profile-planner.cc
        services/first-run-service.cc
        services/settings-gateway-service.cc
        services/settings-profile-service.cc
        services/settings-profile.cc
    INCLUDES
        ../..
    DEPENDS
        argus::clients::settings
        argus::contracts::auth
        argus::contracts::settings
        argus::settings-config
        argus::lib::auth
        argus::lib::config
        argus::lib::errors
        argus::lib::http
        argus::lib::runtime
        argus::lib::validation
        argus::hardware-profile
        Drogon::Drogon)

target_compile_options(argus_settings-gateway PRIVATE -Wall -Wextra)

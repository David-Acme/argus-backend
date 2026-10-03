argus_module(NAME settings-gateway
    SOURCES
        controllers/settings-controller.cc
        dtos/response-list-settings-dto.cc
        dtos/response-owner-catalog-dto.cc
        dtos/response-update-settings-dto.cc
        dtos/update-settings-dto.cc
        services/settings-gateway-service.cc
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
        Drogon::Drogon)

target_compile_options(argus_settings-gateway PRIVATE -Wall -Wextra)

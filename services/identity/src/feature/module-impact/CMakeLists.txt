argus_module(NAME identity-module-impact
    SOURCES
        services/identity-module-impact.cc
        services/identity-role-reassign.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::identity-user
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::errors
        Drogon::Drogon)

target_compile_options(argus_identity-module-impact PRIVATE -Wall -Wextra)

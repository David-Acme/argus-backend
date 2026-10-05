argus_module(NAME identity-enrollment
    SOURCES
        repositories/enrollment/enrollment-repository.cc
        services/enrollment-feature-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-config
        argus::identity-invitation
        argus::identity-shared
        argus::lib::config
        argus::lib::errors
        argus::lib::runtime
        argus::lib::sqlite
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        argus::contracts::voice
        Drogon::Drogon)

target_compile_options(argus_identity-enrollment PRIVATE -Wall -Wextra)

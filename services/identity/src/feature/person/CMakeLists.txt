argus_module(NAME identity-person
    SOURCES
        repositories/person-tag/person-tag-repository.cc
        services/person-feature-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::identity-visitor
        argus::lib::errors
        argus::lib::sqlite
        argus::contracts::auth
        argus::contracts::identity
        argus::contracts::sync
        Drogon::Drogon)

target_compile_options(argus_identity-person PRIVATE -Wall -Wextra)

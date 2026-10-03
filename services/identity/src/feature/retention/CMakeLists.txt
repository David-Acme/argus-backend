argus_module(NAME identity-retention
    SOURCES
        repositories/candidate-retention/candidate-retention-repository.cc
        services/candidate-retention-service.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-config
        argus::identity-shared
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        argus::contracts::sync
        Drogon::Drogon)

target_compile_options(argus_identity-retention PRIVATE -Wall -Wextra)

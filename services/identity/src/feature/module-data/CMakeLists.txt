argus_module(NAME identity-module-data
    SOURCES
        repositories/module-data/module-data-repository.cc
        services/identity-module-data.cc
    INCLUDES ../..
    DEPENDS
        argus::identity-shared
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::runtime
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_identity-module-data PRIVATE -Wall -Wextra)

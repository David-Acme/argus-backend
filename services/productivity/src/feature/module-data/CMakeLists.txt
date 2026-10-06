argus_module(NAME productivity-module-data
    SOURCES
        repositories/module-data/module-data-repository.cc
        services/productivity-module-data.cc
        services/productivity-module-impact.cc
    INCLUDES
        ../..
    DEPENDS
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_productivity-module-data PRIVATE -Wall -Wextra)

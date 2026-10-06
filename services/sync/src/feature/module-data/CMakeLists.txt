argus_module(NAME sync-module-data
    SOURCES
        repositories/module-data/module-data-repository.cc
        services/sync-module-data.cc
    INCLUDES
        ../..
    DEPENDS
        argus::contracts::settings-wire
        argus::contracts::sync
        argus::lib::auth
        argus::lib::sqlite
        argus::lib::text
        Drogon::Drogon)

target_compile_options(argus_sync-module-data PRIVATE -Wall -Wextra)

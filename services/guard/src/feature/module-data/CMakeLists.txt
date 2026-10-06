argus_module(NAME guard-module-data
    SOURCES
        repositories/module-data/module-data-repository.cc
        services/guard-module-data.cc
        services/guard-owner-pin.cc
    INCLUDES
        ../..
    DEPENDS
        argus::guard-safety
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::sqlite
        argus::lib::storage
        Drogon::Drogon)

target_compile_options(argus_guard-module-data PRIVATE -Wall -Wextra)

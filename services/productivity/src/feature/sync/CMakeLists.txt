argus_module(NAME productivity-sync
    SOURCES
        productivity-sync-rpc-service.cc
        repositories/reminder-detail/reminder-detail-repository.cc
        repositories/reminder/reminder-repository.cc
        schemas/reminder-detail/reminder-detail-schema.cc
        schemas/reminder/reminder-schema.cc
    INCLUDES
        ../..
    DEPENDS
        argus::productivity-repositories
        argus::clients::productivity
        argus::contracts::productivity
        argus::contracts::sync
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_productivity-sync PRIVATE -Wall -Wextra)

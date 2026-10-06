argus_module(NAME identity-role-storage
    SOURCES
        services/role-check-migration.cc
    INCLUDES ../..
    DEPENDS
        argus::lib::sqlite
        SQLite::SQLite3
        Drogon::Drogon)

target_compile_options(argus_identity-role-storage PRIVATE -Wall -Wextra)

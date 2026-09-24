add_library(sync-migration STATIC
    sync-migration.cc
)

target_include_directories(sync-migration PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(sync-migration PUBLIC
    ARGUS_SYNC_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../database/schema.sql"
)

target_link_libraries(sync-migration
    PUBLIC
        argus::lib::sqlite
        argus::lib::text
)

if(TARGET SQLite::SQLite3)
    target_link_libraries(sync-migration PUBLIC SQLite::SQLite3)
endif()

target_compile_options(sync-migration PRIVATE -Wall -Wextra)

add_executable(argus-migrate-sync EXCLUDE_FROM_ALL migrate-sync.cc)

target_link_libraries(argus-migrate-sync PRIVATE sync-migration)

target_compile_options(argus-migrate-sync PRIVATE -Wall -Wextra)

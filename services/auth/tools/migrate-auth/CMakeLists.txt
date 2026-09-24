add_library(auth-migration STATIC
    auth-migration.cc
)

target_include_directories(auth-migration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../packages/lib/text/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../packages/lib/sqlite/src
)

target_compile_definitions(auth-migration PUBLIC
    ARGUS_AUTH_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../database/schema.sql"
)

target_link_libraries(auth-migration
    PUBLIC
        Drogon::Drogon
)

if(TARGET SQLite::SQLite3)
    target_link_libraries(auth-migration PUBLIC SQLite::SQLite3)
endif()

target_compile_options(auth-migration PRIVATE -Wall -Wextra)

add_executable(argus-migrate-auth EXCLUDE_FROM_ALL migrate-auth.cc)

target_link_libraries(argus-migrate-auth PRIVATE auth-migration)

target_compile_options(argus-migrate-auth PRIVATE -Wall -Wextra)

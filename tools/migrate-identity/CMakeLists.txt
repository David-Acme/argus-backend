# Identity database migration tool (argus.db -> identity.db). The library
# builds only as a dependency of identity-migration-test; the CLI executable
# is excluded from the default backend build.

add_library(identity-migration STATIC
    identity-migration.cc
)

target_include_directories(identity-migration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRC_ROOT}
)

target_compile_definitions(identity-migration PUBLIC
    ARGUS_IDENTITY_SCHEMA_PATH="${CMAKE_SOURCE_DIR}/database/identity-schema.sql"
)

target_link_libraries(identity-migration
    PUBLIC
        Drogon::Drogon
)

target_compile_options(identity-migration PRIVATE -Wall -Wextra)

add_executable(argus-migrate-identity EXCLUDE_FROM_ALL migrate-identity.cc)

target_link_libraries(argus-migrate-identity PRIVATE identity-migration)

target_compile_options(argus-migrate-identity PRIVATE -Wall -Wextra)
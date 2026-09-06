# Productivity database migration tool (argus.db -> productivity.db). The
# library builds only as a dependency of productivity-migration-test; the CLI
# executable is excluded from the default backend build.

add_library(productivity-migration STATIC
    productivity-migration.cc
)

target_include_directories(productivity-migration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRC_ROOT}
)

target_compile_definitions(productivity-migration PUBLIC
    ARGUS_PRODUCTIVITY_SCHEMA_PATH="${CMAKE_SOURCE_DIR}/database/productivity-schema.sql"
)

target_link_libraries(productivity-migration
    PUBLIC
        Drogon::Drogon
)

if(TARGET SQLite::SQLite3)
    target_link_libraries(productivity-migration PUBLIC SQLite::SQLite3)
endif()

target_compile_options(productivity-migration PRIVATE -Wall -Wextra)

add_executable(argus-migrate-productivity EXCLUDE_FROM_ALL migrate-productivity.cc)

if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(tomlplusplus REQUIRED)
endif()

target_link_libraries(argus-migrate-productivity PRIVATE productivity-migration
                                                         tomlplusplus::tomlplusplus)

target_compile_options(argus-migrate-productivity PRIVATE -Wall -Wextra)
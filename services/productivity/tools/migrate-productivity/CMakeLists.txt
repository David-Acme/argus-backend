add_library(productivity-migration STATIC
    productivity-migration.cc
)

target_include_directories(productivity-migration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../packages/lib/text/src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../packages/lib/sqlite/src
)

target_compile_definitions(productivity-migration PUBLIC
    ARGUS_PRODUCTIVITY_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../database/schema.sql"
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

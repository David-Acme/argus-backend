# Camera database migration tool (argus.db -> camera.db). The library builds
# only as a dependency of camera-migration-test; the CLI executable is
# excluded from the default backend build.

add_library(camera-migration STATIC
    camera-migration.cc
)

target_include_directories(camera-migration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../packages/argus-common/src
)

target_compile_definitions(camera-migration PUBLIC
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../database/schema.sql"
)

target_link_libraries(camera-migration
    PUBLIC
        Drogon::Drogon
)

if(TARGET SQLite::SQLite3)
    target_link_libraries(camera-migration PUBLIC SQLite::SQLite3)
endif()

target_compile_options(camera-migration PRIVATE -Wall -Wextra)

add_executable(argus-migrate-camera EXCLUDE_FROM_ALL migrate-camera.cc)

if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(tomlplusplus REQUIRED)
endif()

target_link_libraries(argus-migrate-camera PRIVATE camera-migration
                                                   tomlplusplus::tomlplusplus)

target_compile_options(argus-migrate-camera PRIVATE -Wall -Wextra)

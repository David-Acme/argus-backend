# Notification database migration tool (argus.db -> notification.db). The
# library builds only as a dependency of notification-migration-test; the CLI
# executable is excluded from the default backend build.

add_library(notification-migration STATIC
    notification-migration.cc
)

target_include_directories(notification-migration
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${SRC_ROOT}
)

target_compile_definitions(notification-migration PUBLIC
    ARGUS_NOTIFICATION_SCHEMA_PATH="${CMAKE_SOURCE_DIR}/database/notification-schema.sql"
)

target_link_libraries(notification-migration
    PUBLIC
        Drogon::Drogon
)

if(TARGET SQLite::SQLite3)
    target_link_libraries(notification-migration PUBLIC SQLite::SQLite3)
endif()

target_compile_options(notification-migration PRIVATE -Wall -Wextra)

add_executable(argus-migrate-notification EXCLUDE_FROM_ALL migrate-notification.cc)

if(NOT TARGET tomlplusplus::tomlplusplus)
    find_package(tomlplusplus REQUIRED)
endif()

target_link_libraries(argus-migrate-notification PRIVATE notification-migration
                                                         tomlplusplus::tomlplusplus)

target_compile_options(argus-migrate-notification PRIVATE -Wall -Wextra)
argus_module(NAME productivity-repositories
    SOURCES
        calendar-event/calendar-event-repository.cc
        calendar-event-share/calendar-event-share-repository.cc
        project/project-repository.cc
        project-member/project-member-repository.cc
        project-task/project-task-repository.cc
        ../schemas/calendar-event/calendar-event-schema.cc
        ../schemas/calendar-event-share/calendar-event-share-schema.cc
        ../schemas/project/project-schema.cc
        ../schemas/project-member/project-member-schema.cc
        ../schemas/project-task/project-task-schema.cc
    INCLUDES
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
    DEPENDS
        argus::contracts::productivity
        argus::contracts::sync
        argus::lib::sqlite
        Drogon::Drogon)

target_compile_options(argus_productivity-repositories PRIVATE -Wall -Wextra)

# argus_sync: the sync WebSocket surface (socket, native sync-table service and
# the sync-path repositories). The legacy backend and the argus-gateway both
# consume this one definition; the gateway never pulls the media objects
# because nothing it links references them.

find_package(Drogon CONFIG REQUIRED)

set(ARGUS_SYNC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/socket/sync/socket/sync-socket.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/socket/sync/services/sync-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../feature/socket/sync/services/synchronized-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/camera/camera-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/camera-stream/camera-stream-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/zone/zone-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/reminder/reminder-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/reminder-detail/reminder-detail-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/calendar-event/calendar-event-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/calendar-event-share/calendar-event-share-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/project/project-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/project-member/project-member-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/project-task/project-task-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/event/event-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/repositories/notification/notification-repository.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/camera/camera-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/camera-stream/camera-stream-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/zone/zone-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/reminder/reminder-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/reminder-detail/reminder-detail-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/calendar-event/calendar-event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/calendar-event-share/calendar-event-share-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/project/project-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/project-member/project-member-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/project-task/project-task-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/event/event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/person-event/person-event-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/schemas/notification/notification-schema.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../shared/services/notification/notification-service.cc
)

add_library(argus_sync STATIC ${ARGUS_SYNC_SOURCES})

target_include_directories(argus_sync
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)

target_link_libraries(argus_sync
    PUBLIC
        argus_identity
        argus::audit
        argus_common
        Drogon::Drogon)

target_compile_options(argus_sync PRIVATE -Wall -Wextra)

# The voice half of the media forwarder left with F6-3: the whole former
# argus_sync_media library is argus-voice's voice-core now; the gateway relays
# the frames to the owner service instead of linking any of this.

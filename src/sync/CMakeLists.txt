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
        argus_common
        Drogon::Drogon)

target_compile_options(argus_sync PRIVATE -Wall -Wextra)

# argus_sync_media: the voice session on the AI stack (F6-2 split the camera
# half out to argus-camera's CameraMediaService). Only the legacy backend links
# it; the gateway relays the frames to each owner service instead.
if(ARGUS_SHERPA_ONNX_TARGET)
    set(ARGUS_SYNC_MEDIA_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/../feature/socket/sync/media/sync-media-service.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../feature/socket/sync/services/voice-engine-seam.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../feature/socket/sync/services/voice-session-service.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/services/stt/remote/stt-remote.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/../shared/services/llm/remote/llm-remote.cc
    )

    add_library(argus_sync_media STATIC ${ARGUS_SYNC_MEDIA_SOURCES})

    target_include_directories(argus_sync_media SYSTEM PUBLIC
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
        ${CMAKE_BINARY_DIR}/third_party/ncnn/src)

    target_link_libraries(argus_sync_media
        PUBLIC
            argus_sync
            ${ARGUS_LLAMA_TARGETS}
            onnxruntime::onnxruntime
            ${ARGUS_NCNN_TARGET}
            ${ARGUS_SHERPA_ONNX_TARGET})

    if(RNNOISE_INCLUDE_DIR AND RNNOISE_LIBRARY)
        target_compile_definitions(argus_sync_media PUBLIC ARGUS_HAS_RNNOISE=1)
        target_include_directories(argus_sync_media SYSTEM PUBLIC
            ${RNNOISE_INCLUDE_DIR})
        target_link_libraries(argus_sync_media PUBLIC ${RNNOISE_LIBRARY})
    else()
        target_compile_definitions(argus_sync_media PUBLIC ARGUS_HAS_RNNOISE=0)
    endif()

    target_compile_options(argus_sync_media PRIVATE -Wall -Wextra)
endif()

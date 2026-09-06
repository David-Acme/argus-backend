find_package(doctest REQUIRED)

set(UNIT_TESTS
    api-response-test
    enums-test
    role-access-test
    thread-budget-test
    validation-dsl-test
    nats-wrapper-test
    sync-change-test
    identity-migration-test
    camera-schema-test
    camera-migration-test
    camera-config-test
    camera-controller-test
    productivity-schema-test
    productivity-migration-test
    notification-schema-test
    notification-migration-test
    productivity-controller-test
    notification-controller-test
    event-intelligence-test
    camera-operator-test
)

foreach(TEST_NAME IN LISTS UNIT_TESTS)
    add_executable(${TEST_NAME} ${TEST_NAME}.cc)

    target_link_libraries(${TEST_NAME} PRIVATE argus_common doctest::doctest)

    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

target_link_libraries(identity-migration-test PRIVATE identity-migration)

# camera-migration builds only as a dependency of the camera unit suites.
target_link_libraries(camera-schema-test PRIVATE camera-migration)
target_link_libraries(camera-migration-test PRIVATE camera-migration)

# productivity-migration and notification-migration build only as dependencies
# of the productivity/notification unit suites.
target_link_libraries(productivity-schema-test PRIVATE productivity-migration)
target_link_libraries(productivity-migration-test PRIVATE productivity-migration)
target_link_libraries(notification-schema-test PRIVATE notification-migration)
target_link_libraries(notification-migration-test PRIVATE notification-migration)

# camera-core (argus-camera service plumbing) is pulled into the default
# backend build only by camera-config-test and camera-controller-test.
target_link_libraries(camera-config-test PRIVATE camera-core)
target_link_libraries(camera-controller-test PRIVATE camera-core)
target_link_libraries(event-intelligence-test PRIVATE camera-core)
target_link_libraries(camera-operator-test PRIVATE camera-core)

# The productivity/notification controller suites compile the same write-side
# feature sources the fase 3 executables compile (shared tree) and ride the
# sync layer for the repositories, schemas and boot plumbing.
set(PRODUCTIVITY_CONTROLLER_SOURCES
    ${SRC_ROOT}/feature/api/calendar-event/controllers/calendar-event-controller.cc
    ${SRC_ROOT}/feature/api/calendar-event/dtos/create-calendar-event-dto.cc
    ${SRC_ROOT}/feature/api/calendar-event/dtos/update-calendar-event-dto.cc
    ${SRC_ROOT}/feature/api/calendar-event/services/calendar-event-feature-service.cc
    ${SRC_ROOT}/feature/api/calendar-event-share/controllers/calendar-event-share-controller.cc
    ${SRC_ROOT}/feature/api/calendar-event-share/dtos/create-calendar-event-share-dto.cc
    ${SRC_ROOT}/feature/api/calendar-event-share/dtos/update-calendar-event-share-dto.cc
    ${SRC_ROOT}/feature/api/calendar-event-share/services/calendar-event-share-feature-service.cc
    ${SRC_ROOT}/feature/api/project/controllers/project-controller.cc
    ${SRC_ROOT}/feature/api/project/dtos/create-project-dto.cc
    ${SRC_ROOT}/feature/api/project/dtos/update-project-dto.cc
    ${SRC_ROOT}/feature/api/project/services/project-feature-service.cc
    ${SRC_ROOT}/feature/api/project-member/controllers/project-member-controller.cc
    ${SRC_ROOT}/feature/api/project-member/dtos/create-project-member-dto.cc
    ${SRC_ROOT}/feature/api/project-member/dtos/update-project-member-dto.cc
    ${SRC_ROOT}/feature/api/project-member/services/project-member-feature-service.cc
    ${SRC_ROOT}/feature/api/project-task/controllers/project-task-controller.cc
    ${SRC_ROOT}/feature/api/project-task/dtos/create-project-task-dto.cc
    ${SRC_ROOT}/feature/api/project-task/dtos/update-project-task-dto.cc
    ${SRC_ROOT}/feature/api/project-task/services/project-task-feature-service.cc
)

set(NOTIFICATION_CONTROLLER_SOURCES
    ${SRC_ROOT}/feature/api/notification/controllers/notification-controller.cc
    ${SRC_ROOT}/feature/api/notification/controllers/notification-token-controller.cc
    ${SRC_ROOT}/feature/api/notification/dtos/notification-read-dto.cc
    ${SRC_ROOT}/feature/api/notification/dtos/register-notification-token-dto.cc
    ${SRC_ROOT}/feature/api/notification/services/notification-feature-service.cc
    ${SRC_ROOT}/feature/api/notification/services/notification-token-feature-service.cc
    ${SRC_ROOT}/shared/repositories/notification-token/notification-token-repository.cc
    ${SRC_ROOT}/shared/schemas/notification-token/notification-token-schema.cc
    ${SRC_ROOT}/shared/services/notification-token/notification-token-service.cc
)

target_sources(productivity-controller-test PRIVATE
    ${PRODUCTIVITY_CONTROLLER_SOURCES})

target_link_libraries(productivity-controller-test PRIVATE argus_sync
    doctest::doctest)

target_sources(notification-controller-test PRIVATE
    ${NOTIFICATION_CONTROLLER_SOURCES})

target_link_libraries(notification-controller-test PRIVATE argus_sync
    doctest::doctest)

add_executable(identity-client-test identity-client-test.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc)

target_link_libraries(identity-client-test PRIVATE argus_common Drogon::Drogon
    sqlite-vec doctest::doctest)

add_test(NAME identity-client-test COMMAND identity-client-test)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# The voice-session seam suite rides the legacy AI stack (argus_sync_media).
if(TARGET argus_sync_media)
    set(VOICE_SEAM_TEST_SOURCES
        voice-session-seam-test.cc
        ${SRC_ROOT}/shared/services/vad/vad-service.cc
        ${SRC_ROOT}/shared/services/noise/noise-suppression-service.cc
        ${SRC_ROOT}/shared/services/reaction/reaction-engine.cc
        ${SRC_ROOT}/shared/services/memory/phrase-catalog.cc
        ${SRC_ROOT}/shared/services/memory/rule-parser.cc
        ${SRC_ROOT}/shared/utils/text-match/phrase-automaton.cc
        ${SRC_ROOT}/shared/wrapper/audio/audio-resampler.cc
        ${SRC_ROOT}/shared/services/stt/stt-service.cc
        ${SRC_ROOT}/shared/services/llm/llm-service.cc
        ${SRC_ROOT}/shared/services/tts/tts-service.cc
        ${SRC_ROOT}/shared/services/tts/tts-engine.cc
        ${SRC_ROOT}/shared/services/tts/onnx-utils.cc
        ${SRC_ROOT}/shared/services/tts/style.cc
        ${SRC_ROOT}/shared/services/tts/unicode-processor.cc
    )

    add_executable(voice-session-seam-test ${VOICE_SEAM_TEST_SOURCES})

    target_link_libraries(voice-session-seam-test PRIVATE argus_sync_media
        doctest::doctest)

    target_compile_options(voice-session-seam-test PRIVATE -Wall -Wextra)

    add_test(NAME voice-session-seam-test COMMAND voice-session-seam-test)
endif()

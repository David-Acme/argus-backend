find_package(doctest REQUIRED)

# Transitional: this tree still compiles shared/config/filter sources by raw
# path (rule-25 debt, tracked for the move to per-service tests/), so it
# needs the whole src/ root itself now that argus_common no longer exports
# it; see argus-common/CONTEXT.md.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../..)

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
    device-credential-test
    cert-san-test
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

# The device credential suite rides the shared identity domain: the DeviceFilter
# gate, the credential repository and the auth-service issuance flow.
target_link_libraries(device-credential-test PRIVATE argus_identity)

# The cert SAN suite rides the shared identity domain too: CertService rotation
# and the in-process TLS listener that proves the hot reload.
target_link_libraries(cert-san-test PRIVATE argus_identity)

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

add_executable(event-sync-empty-test event-sync-empty-test.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc)

target_link_libraries(event-sync-empty-test PRIVATE argus_sync argus_common
    Drogon::Drogon sqlite-vec doctest::doctest)

add_test(NAME event-sync-empty-test COMMAND event-sync-empty-test)

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

# The voice suites ride argus-voice's voice-core (F6-3): the typed gRPC
# session, the remote-only engine seam and the SDK stubs compile there.
add_executable(voice-session-seam-test voice-session-seam-test.cc)

target_link_libraries(voice-session-seam-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-session-seam-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-session-seam-test)

add_test(NAME voice-session-seam-test COMMAND voice-session-seam-test)

# The TTS cutover suite: the voice session speaks through the argus-tts
# internal wire (RemoteVoiceTts) against the in-process fake server.
add_executable(voice-tts-remote-test voice-tts-remote-test.cc)

target_link_libraries(voice-tts-remote-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-tts-remote-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-tts-remote-test)

add_test(NAME voice-tts-remote-test COMMAND voice-tts-remote-test)

# The STT cutover suite: the voice session transcribes through the
# argus-stt internal wire (RemoteVoiceStt), degrading to the existing
# stt_failed error path when the server is down.
add_executable(voice-stt-remote-test voice-stt-remote-test.cc)

target_link_libraries(voice-stt-remote-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-stt-remote-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-stt-remote-test)

add_test(NAME voice-stt-remote-test COMMAND voice-stt-remote-test)

# The LLM cutover suite (F4-5): the voice session streams its replies through
# the argus-llm internal wire (RemoteVoiceLlm), degrading silently when the
# server is down.
add_executable(voice-llm-remote-test voice-llm-remote-test.cc)

target_link_libraries(voice-llm-remote-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-llm-remote-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-llm-remote-test)

add_test(NAME voice-llm-remote-test COMMAND voice-llm-remote-test)

# The argus-stt wire contract (F4-3, Ruling BL): the real engine serves the
# transcribe/config legs from the shared models tree.
add_executable(stt-wire-test stt-wire-test.cc
    ${CMAKE_SOURCE_DIR}/argus-stt/src/controllers/health-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-stt/src/controllers/stt-controller.cc
    ${SRC_ROOT}/shared/services/stt/stt-service.cc)

target_include_directories(stt-wire-test PRIVATE
    ${CMAKE_SOURCE_DIR}/argus-stt/src)

target_include_directories(stt-wire-test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx)

target_compile_definitions(stt-wire-test PRIVATE
    ARGUS_TEST_STT_MODELS_DIR="${CMAKE_SOURCE_DIR}/models/stt")

target_link_libraries(stt-wire-test PRIVATE argus_common
    onnxruntime::onnxruntime ${ARGUS_SHERPA_ONNX_TARGET} doctest::doctest)

target_compile_options(stt-wire-test PRIVATE -Wall -Wextra)

add_test(NAME stt-wire-test COMMAND stt-wire-test)

# The argus-tts wire contract (F4-2, Ruling BH): the real engine serves the
# synthesize/config legs from the shared models tree.
add_executable(tts-wire-test tts-wire-test.cc
    ${SRC_ROOT}/filter/valid-json/valid-json-filter.cc
    ${CMAKE_SOURCE_DIR}/argus-tts/src/controllers/health-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-tts/src/controllers/tts-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-tts/src/tts/synthesize-dto.cc
    ${SRC_ROOT}/shared/services/tts/tts-service.cc
    ${SRC_ROOT}/shared/services/tts/tts-engine.cc
    ${SRC_ROOT}/shared/services/tts/onnx-utils.cc
    ${SRC_ROOT}/shared/services/tts/style.cc
    ${SRC_ROOT}/shared/services/tts/unicode-processor.cc)

target_include_directories(tts-wire-test PRIVATE
    ${CMAKE_SOURCE_DIR}/argus-tts/src)

target_compile_definitions(tts-wire-test PRIVATE
    ARGUS_TEST_TTS_MODELS_DIR="${CMAKE_SOURCE_DIR}/models/tts")

target_link_libraries(tts-wire-test PRIVATE argus_common
    argus::hardware-profile onnxruntime::onnxruntime doctest::doctest)

target_compile_options(tts-wire-test PRIVATE -Wall -Wextra)

add_test(NAME tts-wire-test COMMAND tts-wire-test)

# The camera-talk cutover: speak() reaches the wire before the device,
# degrading to the 502 envelope when argus-tts is down. The camera-control
# feature lives in argus-camera since F6-2; the test compiles it from there.
set(CAMERA_TALK_TEST_SOURCES
    camera-talk-cutover-test.cc
    ${CMAKE_SOURCE_DIR}/argus-camera/src/feature/api/camera-control/controllers/camera-control-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-camera/src/feature/api/camera-control/services/camera-control-feature-service.cc
    ${CMAKE_SOURCE_DIR}/argus-camera/src/feature/api/camera-control/dtos/camera-talk-dto.cc
    ${CMAKE_SOURCE_DIR}/argus-camera/src/feature/api/camera-control/dtos/camera-ptz-dto.cc
    ${CMAKE_SOURCE_DIR}/argus-camera/src/feature/api/camera-control/dtos/camera-preset-dto.cc
    ${CMAKE_SOURCE_DIR}/argus-camera/src/feature/api/camera-control/dtos/camera-settings-dto.cc
    ${SRC_ROOT}/shared/services/camera-driver/camera-driver-registry.cc
    ${SRC_ROOT}/shared/services/camera-driver/tapo-driver.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-crypto.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-http.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-audio.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-ts-muxer.cc
    ${SRC_ROOT}/shared/services/tapo/secure-passthrough-transport.cc
    ${SRC_ROOT}/shared/services/tapo/legacy-stok-transport.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-client.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-api.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-talk-client.cc
    ${SRC_ROOT}/shared/services/tts/remote/tts-remote.cc
    ${SRC_ROOT}/shared/wrapper/audio/audio-resampler.cc
)

add_executable(camera-talk-cutover-test ${CAMERA_TALK_TEST_SOURCES})

target_include_directories(camera-talk-cutover-test PRIVATE
    ${CMAKE_SOURCE_DIR}/argus-camera/src)

target_link_libraries(camera-talk-cutover-test PRIVATE argus_sync argus_common
    OpenSSL::SSL OpenSSL::Crypto doctest::doctest)

target_compile_options(camera-talk-cutover-test PRIVATE -Wall -Wextra)

add_test(NAME camera-talk-cutover-test COMMAND camera-talk-cutover-test)

# The argus-vlm wire contract (F4-4, Ruling BP): the real engine serves the
# describe/config legs from the shared models tree, with the direct-engine
# A/B leg on the same JPEG.
add_executable(vlm-wire-test vlm-wire-test.cc
    ${CMAKE_SOURCE_DIR}/argus-vlm/src/controllers/health-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-vlm/src/controllers/vlm-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-vlm/src/vlm/describe-dto.cc
    ${SRC_ROOT}/shared/services/vision/vision-service.cc)

target_include_directories(vlm-wire-test PRIVATE
    ${CMAKE_SOURCE_DIR}/argus-vlm/src)

target_compile_definitions(vlm-wire-test PRIVATE
    ARGUS_TEST_VLM_MODELS_DIR="${CMAKE_SOURCE_DIR}/models/vision/lfm2vl-25")

target_link_libraries(vlm-wire-test PRIVATE argus_common
    argus::hardware-profile ${ARGUS_LLAMA_TARGETS} opencv::opencv
    doctest::doctest)

target_compile_options(vlm-wire-test PRIVATE -Wall -Wextra)

add_test(NAME vlm-wire-test COMMAND vlm-wire-test)

# The argus-llm wire contract (F4-5, Rulings BT/BS): the real engine serves
# the chat/stream/config legs from the shared models tree, with the sentinel
# framing and the KV-cache counters asserted over the wire.
add_executable(llm-wire-test llm-wire-test.cc
    ${CMAKE_SOURCE_DIR}/argus-llm/src/controllers/health-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-llm/src/controllers/llm-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-llm/src/llm/chat-dto.cc
    ${SRC_ROOT}/shared/services/llm/llm-service.cc)

target_include_directories(llm-wire-test PRIVATE
    ${CMAKE_SOURCE_DIR}/argus-llm/src)

target_compile_definitions(llm-wire-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_SOURCE_DIR}/models/llm")

target_link_libraries(llm-wire-test PRIVATE argus_common
    argus::hardware-profile ${ARGUS_LLAMA_TARGETS} doctest::doctest)

target_compile_options(llm-wire-test PRIVATE -Wall -Wextra)

add_test(NAME llm-wire-test COMMAND llm-wire-test)

# The remote vision adapter (F4-4, Ruling BQ): the JPEG encode, the
# same-keyed caption cache and the error paths against the in-process fake
# argus-vlm server.
add_executable(vision-remote-adapter-test vision-remote-adapter-test.cc
    ${SRC_ROOT}/shared/services/vision/remote/vlm-remote.cc
    ${SRC_ROOT}/shared/services/vision/remote/remote-vision-adapter.cc)

target_link_libraries(vision-remote-adapter-test PRIVATE argus_common
    opencv::opencv doctest::doctest)

target_compile_options(vision-remote-adapter-test PRIVATE -Wall -Wextra)

add_test(NAME vision-remote-adapter-test COMMAND vision-remote-adapter-test)

# The argus-memory wire contract (F4-6, Ruling BY): the real stack serves
# the memory tool surface over the internal wire, with the frozen envelope
# errors and the 503 gate on the unloaded stack.
add_executable(memory-wire-test memory-wire-test.cc)

target_link_libraries(memory-wire-test PRIVATE memory-core doctest::doctest)

target_compile_definitions(memory-wire-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_SOURCE_DIR}/database/memory-schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_SOURCE_DIR}/models/memory"
    ARGUS_TEST_MEMORY_EXTRACT_MODEL="${CMAKE_SOURCE_DIR}/models/extract/NuExtract-1.5-tiny-Q4_K_M.gguf")

target_compile_options(memory-wire-test PRIVATE -Wall -Wextra)

add_test(NAME memory-wire-test COMMAND memory-wire-test)

# The catalog replica replay (Ruling BX): identity and camera change events
# land in the replicas and the gazetteer rebuilds; the snapshot fill only
# fires on empty tables.
add_executable(memory-replica-test memory-replica-test.cc)

target_link_libraries(memory-replica-test PRIVATE memory-core doctest::doctest)

target_compile_definitions(memory-replica-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_SOURCE_DIR}/database/memory-schema.sql")

target_compile_options(memory-replica-test PRIVATE -Wall -Wextra)

add_test(NAME memory-replica-test COMMAND memory-replica-test)

# The queue back-pressure (Ruling BZ): waitForIdle rides the chat port and
# the bounded queue drops derived-data jobs instead of growing.
add_executable(memory-backpressure-test memory-backpressure-test.cc)

target_link_libraries(memory-backpressure-test PRIVATE memory-core
    doctest::doctest)

target_compile_definitions(memory-backpressure-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_SOURCE_DIR}/database/memory-schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_SOURCE_DIR}/models/memory")

target_compile_options(memory-backpressure-test PRIVATE -Wall -Wextra)

add_test(NAME memory-backpressure-test COMMAND memory-backpressure-test)

# The remote memory adapter (Ruling BY): the shared descriptors registered
# onto the tool registry, the wire forwarding and the degrade-without-crash
# paths when argus-memory is down.
add_executable(memory-remote-adapter-test memory-remote-adapter-test.cc
    ${CMAKE_SOURCE_DIR}/argus-common/src/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/memory/remote/memory-remote.cc
    ${SRC_ROOT}/shared/services/memory/remote/remote-memory-service-adapter.cc
    ${SRC_ROOT}/shared/services/memory/memory-tool-descriptors.cc
    ${SRC_ROOT}/shared/services/tools/tool-registry.cc)

target_include_directories(memory-remote-adapter-test PRIVATE ${SRC_ROOT}
    ${CMAKE_SOURCE_DIR}/argus-common/src)

target_link_libraries(memory-remote-adapter-test PRIVATE Drogon::Drogon
    tomlplusplus::tomlplusplus doctest::doctest)

target_compile_options(memory-remote-adapter-test PRIVATE -Wall -Wextra)

add_test(NAME memory-remote-adapter-test COMMAND memory-remote-adapter-test)

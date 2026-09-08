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
    cert-san-test
)

foreach(TEST_NAME IN LISTS UNIT_TESTS)
    add_executable(${TEST_NAME} ${TEST_NAME}.cc)

    target_link_libraries(${TEST_NAME} PRIVATE argus_common doctest::doctest)

    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

# The cert SAN suite rides the shared identity domain too: CertService rotation
# and the in-process TLS listener that proves the hot reload.
target_link_libraries(cert-san-test PRIVATE argus_identity)


add_executable(event-sync-empty-test event-sync-empty-test.cc)

target_link_libraries(event-sync-empty-test PRIVATE argus_sync argus::sqlite
    argus_common Drogon::Drogon sqlite-vec doctest::doctest)

add_test(NAME event-sync-empty-test COMMAND event-sync-empty-test)

add_executable(identity-client-test identity-client-test.cc)

target_link_libraries(identity-client-test PRIVATE argus::sqlite argus_common
    Drogon::Drogon sqlite-vec doctest::doctest)

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

# The TTS wire's fake server moved to the wire module with f7-7a.
target_include_directories(voice-tts-remote-test PRIVATE
    ${CMAKE_SOURCE_DIR}/argus-tts-client/tests/support)

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

target_link_libraries(tts-wire-test PRIVATE argus_common argus::auth
    argus::tts-client argus::hardware-profile onnxruntime::onnxruntime
    doctest::doctest)

target_compile_options(tts-wire-test PRIVATE -Wall -Wextra)

add_test(NAME tts-wire-test COMMAND tts-wire-test)


# The argus-vlm wire contract (F4-4, Ruling BP): the real engine serves the
# describe/config legs from the shared models tree, with the direct-engine
# A/B leg on the same JPEG.
add_executable(vlm-wire-test vlm-wire-test.cc
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
    ${CMAKE_SOURCE_DIR}/argus-llm/src/controllers/llm-controller.cc
    ${CMAKE_SOURCE_DIR}/argus-llm/src/llm/chat-dto.cc
    ${SRC_ROOT}/shared/services/llm/llm-service.cc)

target_include_directories(llm-wire-test PRIVATE
    ${CMAKE_SOURCE_DIR}/argus-llm/src)

target_compile_definitions(llm-wire-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_SOURCE_DIR}/models/llm")

target_link_libraries(llm-wire-test PRIVATE argus_common argus::llm-client
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

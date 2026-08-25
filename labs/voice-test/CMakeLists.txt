set(VOICE_TEST_SRC
    voice-test.cc
    audio.cc
    camera-audio.cc
    ${SRC_ROOT}/shared/services/vad/vad-service.cc
)

set(VOICE_SERVICE_SRC
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/llm/llm-service.cc
    ${SRC_ROOT}/shared/services/stt/stt-service.cc
    ${SRC_ROOT}/shared/services/tts/tts-service.cc
    ${SRC_ROOT}/shared/services/tts/tts-engine.cc
    ${SRC_ROOT}/shared/services/tts/onnx-utils.cc
    ${SRC_ROOT}/shared/services/tts/style.cc
    ${SRC_ROOT}/shared/services/tts/unicode-processor.cc
    ${SRC_ROOT}/shared/services/vision/vision-service.cc
    ${SRC_ROOT}/shared/services/embedding/embedding-service.cc
    ${SRC_ROOT}/shared/services/embedding/unigram-tokenizer.cc
    ${SRC_ROOT}/shared/services/intent/intent-service.cc
    ${SRC_ROOT}/shared/services/memory/memory-service.cc
    ${SRC_ROOT}/shared/services/extract/lexicon-extractor.cc
    ${SRC_ROOT}/shared/services/extract/extraction-service.cc
    ${SRC_ROOT}/shared/services/extract/temporal-resolver.cc
    ${SRC_ROOT}/shared/services/extract/tiered-extractor.cc
    ${SRC_ROOT}/shared/utils/text-match/phrase-automaton.cc


    ${SRC_ROOT}/shared/services/memory/rule-parser.cc
    ${SRC_ROOT}/shared/services/memory/tool-parser.cc
    ${SRC_ROOT}/shared/services/memory/sqlite-graph.cc
    ${SRC_ROOT}/shared/services/memory/entity-resolver.cc
    ${SRC_ROOT}/shared/services/memory/phrase-catalog.cc
    ${SRC_ROOT}/shared/services/reaction/reaction-engine.cc
    ${SRC_ROOT}/shared/services/memory/memory-formation.cc
    ${SRC_ROOT}/shared/services/memory/graph-recall.cc
    ${SRC_ROOT}/shared/repositories/memory-graph/memory-graph-repository.cc
    ${SRC_ROOT}/shared/services/tools/tool-registry.cc
    ${SRC_ROOT}/shared/services/tools/tool-validator.cc
    ${SRC_ROOT}/shared/services/tools/tool-executor.cc
    ${SRC_ROOT}/shared/services/conversation/conversation-service.cc
    ${SRC_ROOT}/shared/services/llm/lfm-adapter.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc
    ${SRC_ROOT}/shared/services/sqlite/vec-db.cc
    ${SRC_ROOT}/shared/repositories/vector-index/vector-index-repository.cc
    ${SRC_ROOT}/shared/utils/schema-runner/schema-runner.cc

    ${SRC_ROOT}/shared/services/stream/camera-audio-source.cc
    ${SRC_ROOT}/shared/services/stream/go2rtc-manager.cc
    ${SRC_ROOT}/shared/services/stream/media-relay.cc
    ${SRC_ROOT}/shared/services/stream/upstream-http.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-audio.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-crypto.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-http.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-talk-client.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-ts-muxer.cc
    ${SRC_ROOT}/shared/utils/text-norm/text-norm.cc
    ${SRC_ROOT}/shared/wrapper/audio/audio-resampler.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
)

add_executable(argus-voice-test
    ${VOICE_TEST_SRC}
    ${VOICE_SERVICE_SRC}
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswscale)

target_include_directories(argus-voice-test
    PRIVATE
        ${SRC_ROOT}
)

target_include_directories(argus-voice-test
    SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
        ${CMAKE_SOURCE_DIR}/third_party/stb
)

target_link_libraries(argus-voice-test
    PRIVATE
        ${ARGUS_NCNN_TARGET}
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        ${ARGUS_LLAMA_TARGETS}
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        opencv::opencv
        ${ARGUS_SHERPA_ONNX_TARGET}
        ${ARGUS_SQLITE_VEC_TARGET}
        ${ARGUS_FASTTEXT_TARGET}
        ${CONAN_LIBS}
        portaudio
        pulse-simple
        pulse
        OpenSSL::SSL
        OpenSSL::Crypto
        PkgConfig::FFMPEG
)

set_target_properties(argus-voice-test PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-voice-test PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-voice-test PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-voice-test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-voice-test>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-voice-test>/models
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:argus-voice-test>/database
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/labs
            $<TARGET_FILE_DIR:argus-voice-test>/labs
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/third_party
            $<TARGET_FILE_DIR:argus-voice-test>/third_party
)

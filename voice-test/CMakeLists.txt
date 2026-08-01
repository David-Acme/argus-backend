set(VOICE_TEST_SRC
    voice-test.cc
    audio.cc
    vad.cc
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
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
)

add_executable(argus-voice-test
    ${VOICE_TEST_SRC}
    ${VOICE_SERVICE_SRC}
)

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
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        llama-cpp::llama-cpp
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        ${ARGUS_SHERPA_ONNX_TARGET}
        ${CONAN_LIBS}
        portaudio
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
)

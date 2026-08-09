set(VOICE_TEST_SRC
    voice-test.cc
    audio.cc
    vad.cc
    camera-audio.cc
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
    ${SRC_ROOT}/shared/services/stream/go2rtc-manager.cc
    ${SRC_ROOT}/shared/services/stream/media-relay.cc
    ${SRC_ROOT}/shared/services/stream/upstream-http.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-audio.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-crypto.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-http.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-talk-client.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-ts-muxer.cc
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
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${CMAKE_SOURCE_DIR}/config.local.toml
            -DDST=$<TARGET_FILE_DIR:argus-voice-test>/config.local.toml
            -P ${CMAKE_SOURCE_DIR}/cmake/copy-if-exists.cmake
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-voice-test>/models
)
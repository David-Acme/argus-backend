set(TAPO_PROBE_SRC
    tapo-probe.cc
)

set(TAPO_SERVICE_SRC
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-crypto.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-http.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-audio.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-ts-muxer.cc
    ${SRC_ROOT}/shared/services/tapo/secure-passthrough-transport.cc
    ${SRC_ROOT}/shared/services/tapo/legacy-stok-transport.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-client.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-api.cc
    ${SRC_ROOT}/shared/services/tapo/tapo-talk-client.cc
    ${SRC_ROOT}/shared/services/tts/tts-service.cc
    ${SRC_ROOT}/shared/services/tts/tts-engine.cc
    ${SRC_ROOT}/shared/services/tts/onnx-utils.cc
    ${SRC_ROOT}/shared/services/tts/style.cc
    ${SRC_ROOT}/shared/services/tts/unicode-processor.cc
    ${SRC_ROOT}/shared/wrapper/audio/audio-resampler.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
)

add_executable(argus-tapo-probe
    ${TAPO_PROBE_SRC}
    ${TAPO_SERVICE_SRC}
)

target_include_directories(argus-tapo-probe
    PRIVATE
        ${SRC_ROOT}
)

target_include_directories(argus-tapo-probe
    SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/stb
)

target_link_libraries(argus-tapo-probe
    PRIVATE
        ${ARGUS_NCNN_TARGET}
        Drogon::Drogon
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        OpenSSL::SSL
        OpenSSL::Crypto
        ${CONAN_LIBS}
)

set_target_properties(argus-tapo-probe PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-tapo-probe PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-tapo-probe PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-tapo-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-tapo-probe>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/labs
            $<TARGET_FILE_DIR:argus-tapo-probe>/labs
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-tapo-probe>/models
)

set(CAMERA_CONTROL_LAB_SRC
    camera-control-lab.cc
)

set(CAMERA_CONTROL_SERVICE_SRC
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
    ${SRC_ROOT}/shared/services/face/face-service.cc
    ${SRC_ROOT}/shared/services/face/face-db.cc
    ${SRC_ROOT}/shared/services/sqlite/vec-db.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc
    ${SRC_ROOT}/shared/repositories/face-embedding/face-embedding-repository.cc
    ${SRC_ROOT}/shared/schemas/face-embedding/face-embedding-schema.cc
    ${SRC_ROOT}/shared/repositories/vector-index/vector-index-repository.cc
    ${SRC_ROOT}/shared/utils/schema-runner/schema-runner.cc
    ${SRC_ROOT}/shared/services/stream/upstream-http.cc
    ${SRC_ROOT}/shared/wrapper/audio/audio-resampler.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
)

add_executable(argus-camera-control-lab
    ${CAMERA_CONTROL_LAB_SRC}
    ${CAMERA_CONTROL_SERVICE_SRC}
)

target_include_directories(argus-camera-control-lab
    PRIVATE
        ${SRC_ROOT}
)

target_include_directories(argus-camera-control-lab
    SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/stb
)

target_link_libraries(argus-camera-control-lab
    PRIVATE
        ${ARGUS_NCNN_TARGET}
        ${ARGUS_SQLITE_VEC_TARGET}
        opencv::opencv
        Drogon::Drogon
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        OpenSSL::SSL
        OpenSSL::Crypto
        ${CONAN_LIBS}
)

set_target_properties(argus-camera-control-lab PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-camera-control-lab PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-camera-control-lab PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-camera-control-lab POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-camera-control-lab>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/labs
            $<TARGET_FILE_DIR:argus-camera-control-lab>/labs
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-camera-control-lab>/models
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:argus-camera-control-lab>/database
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/third_party
            $<TARGET_FILE_DIR:argus-camera-control-lab>/third_party
)

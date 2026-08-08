add_executable(argus-tts-probe
    tts-probe.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/tts/tts-service.cc
    ${SRC_ROOT}/shared/services/tts/tts-engine.cc
    ${SRC_ROOT}/shared/services/tts/onnx-utils.cc
    ${SRC_ROOT}/shared/services/tts/style.cc
    ${SRC_ROOT}/shared/services/tts/unicode-processor.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
)
target_include_directories(argus-tts-probe PRIVATE ${SRC_ROOT})
target_include_directories(argus-tts-probe SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(argus-tts-probe PRIVATE
    Drogon::Drogon onnxruntime::onnxruntime tomlplusplus::tomlplusplus
    ${ARGUS_NCNN_TARGET} ${CONAN_LIBS})
target_compile_options(argus-tts-probe PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>)
target_link_options(argus-tts-probe PRIVATE $<$<CONFIG:Release>:-flto=auto>)
add_custom_command(TARGET argus-tts-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-tts-probe>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-tts-probe>/models)

set(LLM_BENCH_SRC
    llm-bench.cc
)

set(LLM_BENCH_SERVICE_SRC
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/llm/llm-service.cc
    ${SRC_ROOT}/shared/services/vision/vision-service.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
)

add_executable(argus-llm-bench
    ${LLM_BENCH_SRC}
    ${LLM_BENCH_SERVICE_SRC}
)

target_include_directories(argus-llm-bench
    PRIVATE
        ${SRC_ROOT}
)

target_include_directories(argus-llm-bench
    SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/stb
)

target_link_libraries(argus-llm-bench
    PRIVATE
        Drogon::Drogon
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        ${ARGUS_LLAMA_TARGETS}
        opencv::opencv
        ${ARGUS_NCNN_TARGET}
        ${CONAN_LIBS}
)

set_target_properties(argus-llm-bench PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-llm-bench PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-llm-bench PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-llm-bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-llm-bench>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-llm-bench>/models
)

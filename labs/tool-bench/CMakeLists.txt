set(TOOL_BENCH_SRC
    tool-bench.cc
)

set(TOOL_BENCH_SERVICE_SRC
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/llm/llm-service.cc
    ${SRC_ROOT}/shared/services/llm/lfm-adapter.cc
    ${SRC_ROOT}/shared/services/tools/tool-registry.cc
    ${SRC_ROOT}/shared/services/tools/tool-validator.cc
    ${SRC_ROOT}/shared/services/tools/tool-executor.cc
    ${SRC_ROOT}/shared/services/intent/intent-service.cc
    ${SRC_ROOT}/shared/utils/text-norm/text-norm.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
)

add_executable(argus-tool-bench
    ${TOOL_BENCH_SRC}
    ${TOOL_BENCH_SERVICE_SRC}
)

target_include_directories(argus-tool-bench
    PRIVATE
        ${SRC_ROOT}
)

target_include_directories(argus-tool-bench
    SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/stb
)

target_link_libraries(argus-tool-bench
    PRIVATE
        ${ARGUS_FASTTEXT_TARGET}
        Drogon::Drogon
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        ${ARGUS_LLAMA_TARGETS}
        ${CONAN_LIBS}
)

set_target_properties(argus-tool-bench PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-tool-bench PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-tool-bench PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-tool-bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-tool-bench>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-tool-bench>/models
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/labs
            $<TARGET_FILE_DIR:argus-tool-bench>/labs
)

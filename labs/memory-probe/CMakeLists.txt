set(MEMORY_PROBE_SRC
    memory-probe.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc
    ${SRC_ROOT}/shared/services/sqlite/vec-db.cc
    ${SRC_ROOT}/shared/services/embedding/embedding-service.cc
    ${SRC_ROOT}/shared/services/embedding/unigram-tokenizer.cc
    ${SRC_ROOT}/shared/services/memory/memory-service.cc
    ${SRC_ROOT}/shared/services/memory/memory-recall.cc
    ${SRC_ROOT}/shared/services/memory/memory-store.cc
    ${SRC_ROOT}/shared/services/memory/rule-parser.cc
    ${SRC_ROOT}/shared/services/memory/tool-parser.cc
    ${SRC_ROOT}/shared/services/memory/simhash.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
)

add_executable(argus-memory-probe
    ${MEMORY_PROBE_SRC}
)

target_include_directories(argus-memory-probe
    PRIVATE
        ${SRC_ROOT}
)

target_link_libraries(argus-memory-probe
    PRIVATE
        ${ARGUS_SQLITE_VEC_TARGET}
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        ${ARGUS_SHERPA_ONNX_TARGET}
        ${CONAN_LIBS}
)

set_target_properties(argus-memory-probe PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-memory-probe PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

add_custom_command(
    TARGET argus-memory-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-memory-probe>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:argus-memory-probe>/database
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-memory-probe>/models
)

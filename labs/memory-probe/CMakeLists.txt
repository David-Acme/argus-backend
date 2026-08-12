set(MEMORY_PROBE_SRC
    memory-probe.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc
    ${SRC_ROOT}/shared/services/sqlite/vec-db.cc
    ${SRC_ROOT}/shared/repositories/vector-index/vector-index-repository.cc
    ${SRC_ROOT}/shared/utils/schema-runner/schema-runner.cc

    ${SRC_ROOT}/shared/services/embedding/embedding-service.cc
    ${SRC_ROOT}/shared/services/embedding/unigram-tokenizer.cc
    ${SRC_ROOT}/shared/services/llm/llm-service.cc
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
    ${SRC_ROOT}/shared/repositories/memory-phrase/memory-phrase-repository.cc
    ${SRC_ROOT}/shared/repositories/memory-lexicon/memory-lexicon-repository.cc
    ${SRC_ROOT}/shared/services/memory/memory-formation.cc
    ${SRC_ROOT}/shared/services/memory/graph-recall.cc
    ${SRC_ROOT}/shared/repositories/memory-graph/memory-graph-repository.cc
    ${SRC_ROOT}/shared/services/tools/tool-registry.cc
    ${SRC_ROOT}/shared/services/tools/tool-validator.cc
    ${SRC_ROOT}/shared/services/tools/tool-executor.cc
    ${SRC_ROOT}/shared/utils/text-norm/text-norm.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
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
        ${ARGUS_LLAMA_TARGETS}
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

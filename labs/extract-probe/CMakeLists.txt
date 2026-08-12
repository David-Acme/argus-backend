set(EXTRACT_PROBE_SRC
    extract-probe.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/extract/lexicon-extractor.cc
    ${SRC_ROOT}/shared/services/extract/extraction-service.cc
    ${SRC_ROOT}/shared/services/extract/temporal-resolver.cc
    ${SRC_ROOT}/shared/services/extract/tiered-extractor.cc
    ${SRC_ROOT}/shared/services/memory/sqlite-graph.cc
    ${SRC_ROOT}/shared/repositories/memory-graph/memory-graph-repository.cc
    ${SRC_ROOT}/shared/repositories/memory-lexicon/memory-lexicon-repository.cc
    ${SRC_ROOT}/shared/utils/schema-runner/schema-runner.cc
    ${SRC_ROOT}/shared/utils/text-norm/text-norm.cc
    ${SRC_ROOT}/shared/utils/text-match/phrase-automaton.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
)

add_executable(argus-extract-probe
    ${EXTRACT_PROBE_SRC}
)

target_include_directories(argus-extract-probe
    PRIVATE
        ${SRC_ROOT}
)

target_link_libraries(argus-extract-probe
    PRIVATE
        ${ARGUS_LLAMA_TARGETS}
        Drogon::Drogon
        tomlplusplus::tomlplusplus
        ${CONAN_LIBS}
)

set_target_properties(argus-extract-probe PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-extract-probe PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-extract-probe PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-extract-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-extract-probe>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-extract-probe>/models
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:argus-extract-probe>/database
)

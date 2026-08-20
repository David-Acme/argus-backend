set(INTENT_PROBE_SRC
    intent-probe.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/intent/intent-service.cc
    ${SRC_ROOT}/shared/services/memory/phrase-catalog.cc
    ${SRC_ROOT}/shared/services/memory/rule-parser.cc
    ${SRC_ROOT}/shared/utils/text-match/phrase-automaton.cc
    ${SRC_ROOT}/shared/utils/text-norm/text-norm.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
)

add_executable(argus-intent-probe
    ${INTENT_PROBE_SRC}
)

target_include_directories(argus-intent-probe
    PRIVATE
        ${SRC_ROOT}
)

target_link_libraries(argus-intent-probe
    PRIVATE
        ${ARGUS_FASTTEXT_TARGET}
        Drogon::Drogon
        tomlplusplus::tomlplusplus
        ${CONAN_LIBS}
)

set_target_properties(argus-intent-probe PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-intent-probe PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-intent-probe PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-intent-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-intent-probe>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-intent-probe>/models
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/labs
            $<TARGET_FILE_DIR:argus-intent-probe>/labs
)

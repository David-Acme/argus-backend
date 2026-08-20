set(REACTION_PROBE_SRC
    reaction-probe.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/memory/phrase-catalog.cc
    ${SRC_ROOT}/shared/services/memory/rule-parser.cc
    ${SRC_ROOT}/shared/services/reaction/reaction-engine.cc
    ${SRC_ROOT}/shared/utils/text-match/phrase-automaton.cc
)

add_executable(argus-reaction-probe
    ${REACTION_PROBE_SRC}
)

target_include_directories(argus-reaction-probe
    PRIVATE
        ${SRC_ROOT}
)

target_link_libraries(argus-reaction-probe
    PRIVATE
        Drogon::Drogon
        tomlplusplus::tomlplusplus
        ${CONAN_LIBS}
)

set_target_properties(argus-reaction-probe PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-reaction-probe PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-reaction-probe PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-reaction-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-reaction-probe>/config.toml
)

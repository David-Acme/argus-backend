set(QUEUE_PROBE_SRC
    queue-probe.cc
    ${SRC_ROOT}/shared/repositories/job/job-repository.cc
    ${SRC_ROOT}/shared/utils/schema-runner/schema-runner.cc
    ${SRC_ROOT}/shared/services/queue/job-queue.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
)

add_executable(argus-queue-probe
    ${QUEUE_PROBE_SRC}
)

target_include_directories(argus-queue-probe
    PRIVATE
        ${SRC_ROOT}
)

target_link_libraries(argus-queue-probe
    PRIVATE
        Drogon::Drogon
        ${CONAN_LIBS}
)

set_target_properties(argus-queue-probe PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-queue-probe PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-queue-probe PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-queue-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:argus-queue-probe>/database
)

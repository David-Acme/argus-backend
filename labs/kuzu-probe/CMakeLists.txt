set(KUZU_PROBE_SRC
    kuzu-probe.cc
    kuzu-db.cc
)

add_executable(argus-kuzu-probe
    ${KUZU_PROBE_SRC}
)

target_include_directories(argus-kuzu-probe
    PRIVATE
        ${SRC_ROOT}
)

target_link_libraries(argus-kuzu-probe
    PRIVATE
        ${ARGUS_KUZU_TARGET}
        Drogon::Drogon
)

set_target_properties(argus-kuzu-probe PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# The probe is the kuzu gate reproducer: it is NOT part of the default build
# (a full kuzu compile is ~40 min). Build it explicitly when needed:
#   cmake --build --preset dev -j 8 --target argus-kuzu-probe
set_target_properties(argus-kuzu-probe PROPERTIES EXCLUDE_FROM_ALL TRUE)

target_compile_options(argus-kuzu-probe PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-kuzu-probe PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-kuzu-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:argus-kuzu-probe>/database
)

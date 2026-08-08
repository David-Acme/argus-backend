add_executable(argus-stream-probe
    stream-probe.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/stream/go2rtc-manager.cc
    ${SRC_ROOT}/shared/services/stream/stream-hub.cc
    ${SRC_ROOT}/shared/services/stream/upstream-http.cc
    ${SRC_ROOT}/shared/services/stream/ws-frame.cc
)
target_include_directories(argus-stream-probe PRIVATE ${SRC_ROOT})
target_link_libraries(argus-stream-probe PRIVATE
    Drogon::Drogon tomlplusplus::tomlplusplus ${CONAN_LIBS})
target_compile_options(argus-stream-probe PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>)
target_link_options(argus-stream-probe PRIVATE $<$<CONFIG:Release>:-flto=auto>)
add_custom_command(TARGET argus-stream-probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-stream-probe>/config.toml)

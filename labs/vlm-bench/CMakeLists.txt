add_executable(argus-vlm-bench
    vlm-bench.cc
)

target_include_directories(argus-vlm-bench
    PRIVATE
        ${SRC_ROOT}
)

target_link_libraries(argus-vlm-bench
    PRIVATE
        ${ARGUS_LLAMA_TARGETS}
        opencv::opencv
        ${CONAN_LIBS}
)

set_target_properties(argus-vlm-bench PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

target_compile_options(argus-vlm-bench PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)

target_link_options(argus-vlm-bench PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET argus-vlm-bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-vlm-bench>/models
)

add_executable(argus-vision-check
    service-check.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/services/vision/vision-service.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
)
target_include_directories(argus-vision-check PRIVATE ${SRC_ROOT})
target_include_directories(argus-vision-check SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party ${CMAKE_SOURCE_DIR}/third_party/stb)
target_link_libraries(argus-vision-check PRIVATE
    Drogon::Drogon tomlplusplus::tomlplusplus ${ARGUS_LLAMA_TARGETS}
    opencv::opencv ${ARGUS_NCNN_TARGET} ${CONAN_LIBS})
target_compile_options(argus-vision-check PRIVATE -Wall -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>)
target_link_options(argus-vision-check PRIVATE $<$<CONFIG:Release>:-flto=auto>)
add_custom_command(TARGET argus-vision-check POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:argus-vision-check>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:argus-vision-check>/models)

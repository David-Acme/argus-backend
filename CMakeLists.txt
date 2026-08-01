cmake_minimum_required(VERSION 3.25)
project(argus-backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR} ${CMAKE_PREFIX_PATH})

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(OpenCV CONFIG REQUIRED)
find_package(Eigen3 REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)
find_package(tomlplusplus REQUIRED)

add_subdirectory(third_party)

# llama.cpp (LLM inference) is vendored as a git submodule pinned to a
# release with the LFM2.5-1.2B architecture support.
set(LLAMA_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE ON CACHE BOOL "" FORCE)
set(GGML_LLAMAFILE ON CACHE BOOL "" FORCE)
set(GGML_CPU_REPACK ON CACHE BOOL "" FORCE)
set(GGML_OPENMP ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/llama.cpp EXCLUDE_FROM_ALL)
set(CMAKE_CXX_STANDARD 20)

file(GLOB_RECURSE CONFIG_SRC
    ${SRC_ROOT}/config/*.cc
)

file(GLOB_RECURSE API_SRC
    ${SRC_ROOT}/feature/api/**/*.cc
)

file(GLOB_RECURSE SOCKET_SRC
    ${SRC_ROOT}/feature/socket/**/*.cc
)

file(GLOB_RECURSE FILTER_SRC
    ${SRC_ROOT}/filter/**/*.cc
)

file(GLOB_RECURSE SHARED_SRC
    ${SRC_ROOT}/shared/**/*.cc
)

file(GLOB_RECURSE TEST_SRC
    ${SRC_ROOT}/test/*.cc
    ${SRC_ROOT}/test/**/*.cc
)

add_executable(${PROJECT_NAME}
    ${SRC_ROOT}/main.cc
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${SRC_ROOT}
        ${CMAKE_BINARY_DIR}/third_party/ncnn/src
)

target_include_directories(${PROJECT_NAME}
    SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
        ${CMAKE_SOURCE_DIR}/third_party/ncnn/src
)

target_sources(${PROJECT_NAME}
    PRIVATE
        ${CONFIG_SRC}
        ${API_SRC}
        ${FILTER_SRC}
        ${SHARED_SRC}
        ${SOCKET_SRC}
        ${TEST_SRC}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json

        llama
        opencv::opencv
        Eigen3::Eigen
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        ${ARGUS_FASTTEXT_TARGET}
        ${ARGUS_NCNN_TARGET}
        ${ARGUS_SHERPA_ONNX_TARGET}
        ${CONAN_LIBS}
)

if(ARGUS_BUILD_INSPIREFACE AND TARGET InspireFace)
  target_link_libraries(${PROJECT_NAME} PRIVATE InspireFace)
endif()

if(ARGUS_SHERPA_ONNX_TARGET AND TARGET ${ARGUS_SHERPA_ONNX_TARGET})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${ARGUS_SHERPA_ONNX_TARGET})
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# Machine-tuned Release: -march=native is resolved at build time on the
# deployment machine, so binaries stay optimal on any hardware.
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)
target_link_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/models
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/database
)

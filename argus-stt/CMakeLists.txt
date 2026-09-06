cmake_minimum_required(VERSION 3.25)
project(argus-stt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(STT_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure).
    find_package(Drogon CONFIG REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
endif()

find_package(doctest REQUIRED)

# Foundation slice instead of argus_common: argus_common PUBLIC-links ncnn
# (face-era hardware probing) and would drag ncnn symbols into this binary,
# breaking the zero-AI-symbols rule (Ruling BK). Same pattern as the F4-2
# tts-foundation slice.
add_library(stt-foundation STATIC
    ${SHARED_ROOT}/wrapper/api-response/api-response.cc
    ${SHARED_ROOT}/wrapper/thread-budget/thread-budget.cc
    ${SHARED_ROOT}/wrapper/hardware-profile/hardware-profile.cc
    ${SHARED_ROOT}/services/config-service/config-service.cc
)
target_include_directories(stt-foundation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(stt-foundation PRIVATE ARGUS_NO_NCNN_GPU=1)
target_link_libraries(stt-foundation PUBLIC Drogon::Drogon
    tomlplusplus::tomlplusplus)
target_compile_options(stt-foundation PRIVATE -Wall -Wextra)

# sherpa-onnx is THE capacity of this service (F4-3, Ruling BK): no face/llm/
# vlm/tts/vad code is compiled, so the binary carries no other AI symbols.
if(NOT TARGET sherpa-onnx-c-api)
    # Standalone build: integrate the submodule exactly as the root
    # third_party does, redirecting sherpa's FetchContent onnxruntime to the
    # Conan-provided target.
    set(json_POPULATED TRUE CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_CHECK OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_JNI OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_GPU OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
    set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
    get_target_property(ARGUS_STT_ONNX_INCLUDE_DIR onnxruntime::onnxruntime
                        INTERFACE_INCLUDE_DIRECTORIES)
    set(location_onnxruntime_header_dir "${ARGUS_STT_ONNX_INCLUDE_DIR}")
    set(location_onnxruntime_lib "/__sherpa_use_conan_onnxruntime__")
    set(onnxruntime_lib_files "")
    add_library(onnxruntime INTERFACE IMPORTED)
    target_include_directories(onnxruntime INTERFACE
        "${ARGUS_STT_ONNX_INCLUDE_DIR}")
    target_link_libraries(onnxruntime INTERFACE onnxruntime::onnxruntime)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sherpa-onnx
                     sherpa-onnx-build EXCLUDE_FROM_ALL)
endif()

add_library(stt-core STATIC
    ${STT_SRC_ROOT}/controllers/health-controller.cc
    ${STT_SRC_ROOT}/controllers/stt-controller.cc
    ${STT_SRC_ROOT}/server/listener-config.cc
    # Boot plumbing shared with the backend: the centralized response config
    # (envelope/404/405).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/app-config.cc
    # The engine compiled from the shared tree (PORTED pattern), so the
    # legacy binary and this service transcribe identically.
    ${SHARED_ROOT}/services/stt/stt-service.cc
)

target_include_directories(stt-core SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sherpa-onnx)

target_include_directories(stt-core PUBLIC
    ${STT_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(stt-core PUBLIC
    stt-foundation
    Drogon::Drogon
    onnxruntime::onnxruntime
    nlohmann_json::nlohmann_json
    sherpa-onnx-c-api)

target_compile_options(stt-core PRIVATE -Wall -Wextra)

add_executable(argus-stt ${STT_SRC_ROOT}/main.cc)

target_link_libraries(argus-stt PRIVATE stt-core)

target_compile_options(argus-stt PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set_target_properties(argus-stt PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

add_custom_command(TARGET argus-stt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../models
            $<TARGET_FILE_DIR:argus-stt>/models)

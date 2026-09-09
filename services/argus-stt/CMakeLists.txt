cmake_minimum_required(VERSION 3.25)
project(argus-stt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(STT_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${STT_SRC_ROOT}/shared)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure).
    find_package(Drogon CONFIG REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
endif()

find_package(doctest REQUIRED)

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
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sherpa-onnx
                     sherpa-onnx-build EXCLUDE_FROM_ALL)
endif()

add_library(stt-core STATIC
    ${STT_SRC_ROOT}/controllers/stt-controller.cc
    # The engine compiled from the shared tree (PORTED pattern), so the
    # legacy binary and this service transcribe identically.
    ${SHARED_ROOT}/services/stt/stt-service.cc
)

target_include_directories(stt-core SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sherpa-onnx)

target_include_directories(stt-core PUBLIC
    ${STT_SRC_ROOT})

target_link_libraries(stt-core PUBLIC
    argus_common
    argus::stt-client
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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models
            $<TARGET_FILE_DIR:argus-stt>/models)

# The wire suite opts back into the default build: this folder is added
# EXCLUDE_FROM_ALL.
enable_testing()
find_package(doctest REQUIRED)


# The argus-stt wire contract (F4-3, Ruling BL): the real engine serves the
# transcribe/config legs from the shared models tree.
add_executable(stt-wire-test tests/unit/stt-wire-test.cc
    ${STT_SRC_ROOT}/controllers/stt-controller.cc
    ${SHARED_ROOT}/services/stt/stt-service.cc)

target_include_directories(stt-wire-test PRIVATE
    ${STT_SRC_ROOT})

target_include_directories(stt-wire-test SYSTEM PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx)

target_compile_definitions(stt-wire-test PRIVATE
    ARGUS_TEST_STT_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/stt")

target_link_libraries(stt-wire-test PRIVATE argus_common argus::stt-client
    onnxruntime::onnxruntime ${ARGUS_SHERPA_ONNX_TARGET} doctest::doctest)

target_include_directories(stt-wire-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-stt-client/tests/support)

target_compile_options(stt-wire-test PRIVATE -Wall -Wextra)

set_target_properties(stt-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME stt-wire-test COMMAND stt-wire-test)

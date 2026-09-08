cmake_minimum_required(VERSION 3.25)
project(argus-tts VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TTS_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure).
    find_package(Drogon CONFIG REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
endif()

find_package(doctest REQUIRED)

# onnxruntime is THE capacity of this service (F4-2, Ruling BG): no face/llm/
# vlm/stt/vad code is compiled, so the binary carries no other AI symbols.
add_library(tts-core STATIC
    ${TTS_SRC_ROOT}/controllers/tts-controller.cc
    ${TTS_SRC_ROOT}/tts/synthesize-dto.cc
    # The engine compiled from the shared tree (F2-1 PORTED pattern), so the
    # legacy binary and this service synthesize byte-identically.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/tts/tts-service.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/tts/tts-engine.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/tts/onnx-utils.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/tts/style.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared/services/tts/unicode-processor.cc
)

target_include_directories(tts-core PUBLIC
    ${TTS_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(tts-core PUBLIC
    argus_common
    argus::auth
    argus::hardware-profile
    Drogon::Drogon
    onnxruntime::onnxruntime
    nlohmann_json::nlohmann_json)

target_compile_options(tts-core PRIVATE -Wall -Wextra)

add_executable(argus-tts ${TTS_SRC_ROOT}/main.cc)

target_link_libraries(argus-tts PRIVATE tts-core)

target_compile_options(argus-tts PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set_target_properties(argus-tts PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

add_custom_command(TARGET argus-tts POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../models
            $<TARGET_FILE_DIR:argus-tts>/models)

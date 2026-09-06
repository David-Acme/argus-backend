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

# Foundation slice instead of argus_common: argus_common PUBLIC-links ncnn
# (face-era hardware probing) and would drag ncnn symbols into this binary,
# breaking the zero-AI-symbols rule (Ruling BG).
add_library(tts-foundation STATIC
    ${SHARED_ROOT}/wrapper/api-response/api-response.cc
    ${SHARED_ROOT}/wrapper/thread-budget/thread-budget.cc
    ${SHARED_ROOT}/wrapper/hardware-profile/hardware-profile.cc
    ${SHARED_ROOT}/services/config-service/config-service.cc
)
target_include_directories(tts-foundation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(tts-foundation PRIVATE ARGUS_NO_NCNN_GPU=1)
target_link_libraries(tts-foundation PUBLIC Drogon::Drogon
    tomlplusplus::tomlplusplus)
target_compile_options(tts-foundation PRIVATE -Wall -Wextra)

# onnxruntime is THE capacity of this service (F4-2, Ruling BG): no face/llm/
# vlm/stt/vad code is compiled, so the binary carries no other AI symbols.
add_library(tts-core STATIC
    ${TTS_SRC_ROOT}/controllers/health-controller.cc
    ${TTS_SRC_ROOT}/controllers/tts-controller.cc
    ${TTS_SRC_ROOT}/server/listener-config.cc
    ${TTS_SRC_ROOT}/tts/synthesize-dto.cc
    # Boot plumbing shared with the backend: the centralized response config
    # (envelope/404/405) and the JSON-body filter it reports through.
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/app-config.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/filter/valid-json/valid-json-filter.cc
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
    tts-foundation
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

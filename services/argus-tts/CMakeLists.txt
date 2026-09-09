cmake_minimum_required(VERSION 3.25)
project(argus-tts VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TTS_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${TTS_SRC_ROOT}/shared)

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
    ${TTS_SRC_ROOT}/shared/services/tts/tts-service.cc
    ${TTS_SRC_ROOT}/shared/services/tts/tts-engine.cc
    ${TTS_SRC_ROOT}/shared/services/tts/onnx-utils.cc
    ${TTS_SRC_ROOT}/shared/services/tts/style.cc
    ${TTS_SRC_ROOT}/shared/services/tts/unicode-processor.cc
)

target_include_directories(tts-core PUBLIC
    ${TTS_SRC_ROOT})

target_link_libraries(tts-core PUBLIC
    argus_common
    argus::auth
    argus::tts-client
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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models
            $<TARGET_FILE_DIR:argus-tts>/models)

# The wire suite is a root-project test; it opts back into the default
# build because this folder is added EXCLUDE_FROM_ALL.
if(ARGUS_ROOT_PROJECT)
    find_package(doctest REQUIRED)


    # The argus-tts wire contract (F4-2, Ruling BH): the real engine serves the
    # synthesize/config legs from the shared models tree.
    add_executable(tts-wire-test tests/unit/tts-wire-test.cc
        ${TTS_SRC_ROOT}/controllers/tts-controller.cc
        ${TTS_SRC_ROOT}/tts/synthesize-dto.cc
        ${SHARED_ROOT}/services/tts/tts-service.cc
        ${SHARED_ROOT}/services/tts/tts-engine.cc
        ${SHARED_ROOT}/services/tts/onnx-utils.cc
        ${SHARED_ROOT}/services/tts/style.cc
        ${SHARED_ROOT}/services/tts/unicode-processor.cc)

    target_include_directories(tts-wire-test PRIVATE
        ${TTS_SRC_ROOT})

    target_compile_definitions(tts-wire-test PRIVATE
        ARGUS_TEST_TTS_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/tts")

    target_link_libraries(tts-wire-test PRIVATE argus_common argus::auth
        argus::tts-client argus::hardware-profile onnxruntime::onnxruntime
        doctest::doctest)

    target_include_directories(tts-wire-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/argus-tts-client/tests/support)

    target_compile_options(tts-wire-test PRIVATE -Wall -Wextra)

    set_target_properties(tts-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME tts-wire-test COMMAND tts-wire-test)
endif()

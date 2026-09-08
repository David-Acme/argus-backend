cmake_minimum_required(VERSION 3.25)
project(argus-voice VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

if(NOT TARGET argus_common)
    # Standalone build: reuse the root project's shared foundation so the
    # voice service links the same objects as the root-tree build.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_path(RNNOISE_INCLUDE_DIR NAMES rnnoise.h)
    find_library(RNNOISE_LIBRARY NAMES rnnoise)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-common
                     ${CMAKE_BINARY_DIR}/argus-common)
endif()

if(NOT TARGET argus::sdk-voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-contracts
                     ${CMAKE_BINARY_DIR}/argus-contracts EXCLUDE_FROM_ALL)
endif()

find_package(doctest REQUIRED)

# The voice engine set compiled from the shared tree (PORTED pattern): the
# vad/noise/reaction/resampler stack plus the remote stt/tts/llm clients;
# argus-voice is remote-only, so the in-process stt/tts/llm engines do not
# compile here.
set(VOICE_ENGINE_SOURCES
    ../src/shared/services/vad/vad-service.cc
    ../src/shared/services/noise/noise-suppression-service.cc
    ../src/shared/services/reaction/reaction-engine.cc
    ../src/shared/services/stt/remote/stt-remote.cc
)

argus_module(NAME voice-core
    SOURCES
        src/feature/voice/voice-session-service.cc
        src/feature/voice/voice-engine-seam.cc
        src/feature/voice/voice-rpc-service.cc
        src/feature/health/health-rpc-service.cc
        ${VOICE_ENGINE_SOURCES}
    INCLUDES
        src
        ../src
    DEPENDS
        argus::sdk-voice
        argus::sdk-health
        argus::sdk-identity
        argus::audio
        argus::tts-client
        argus::llm-client
        argus::phrase
        argus_common
    SYSTEM_DEPENDS
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        onnxruntime::onnxruntime
)

# Third-party headers are SYSTEM (AGENTS.md rule 15).
target_include_directories(argus_voice-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/stb)

if(RNNOISE_INCLUDE_DIR AND RNNOISE_LIBRARY)
    target_compile_definitions(argus_voice-core PUBLIC ARGUS_HAS_RNNOISE=1)
    target_include_directories(argus_voice-core SYSTEM PUBLIC
        ${RNNOISE_INCLUDE_DIR})
    target_link_libraries(argus_voice-core PUBLIC ${RNNOISE_LIBRARY})
else()
    target_compile_definitions(argus_voice-core PUBLIC ARGUS_HAS_RNNOISE=0)
endif()

target_compile_options(argus_voice-core PRIVATE -Wall -Wextra)

argus_service(NAME argus-voice
    MAIN src/main.cc
    MODULES argus_voice-core
    PORTS 7034 7035)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

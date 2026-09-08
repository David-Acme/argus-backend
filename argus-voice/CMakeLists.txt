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
    src/shared/services/vad/vad-service.cc
    src/shared/services/noise/noise-suppression-service.cc
    src/shared/services/reaction/reaction-engine.cc
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
        argus::stt-client
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

# The voice suites are root-project tests; the targets opt back into the
# default build because this folder is added EXCLUDE_FROM_ALL.
if(ARGUS_ROOT_PROJECT)
    # The voice suites ride argus-voice's voice-core (F6-3): the typed gRPC
    # session, the remote-only engine seam and the SDK stubs compile there.
    add_executable(voice-session-seam-test tests/unit/voice-session-seam-test.cc)

    target_link_libraries(voice-session-seam-test PRIVATE argus::voice-core
        doctest::doctest)

    target_compile_options(voice-session-seam-test PRIVATE -Wall -Wextra)

    argus_runtime_rpath(voice-session-seam-test)

    set_target_properties(voice-session-seam-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME voice-session-seam-test COMMAND voice-session-seam-test)

    # The TTS cutover suite: the voice session speaks through the argus-tts
    # internal wire (RemoteVoiceTts) against the in-process fake server.
    add_executable(voice-tts-remote-test tests/unit/voice-tts-remote-test.cc)

    # The TTS wire's fake server moved to the wire module with f7-7a.
    target_include_directories(voice-tts-remote-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../argus-tts-client/tests/support)

    target_link_libraries(voice-tts-remote-test PRIVATE argus::voice-core
        doctest::doctest)

    target_compile_options(voice-tts-remote-test PRIVATE -Wall -Wextra)

    argus_runtime_rpath(voice-tts-remote-test)

    set_target_properties(voice-tts-remote-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME voice-tts-remote-test COMMAND voice-tts-remote-test)

    # The STT cutover suite: the voice session transcribes through the
    # argus-stt internal wire (RemoteVoiceStt), degrading to the existing
    # stt_failed error path when the server is down.
    add_executable(voice-stt-remote-test tests/unit/voice-stt-remote-test.cc)

    target_include_directories(voice-stt-remote-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../argus-stt-client/tests/support)

    target_link_libraries(voice-stt-remote-test PRIVATE argus::voice-core
        doctest::doctest)

    target_compile_options(voice-stt-remote-test PRIVATE -Wall -Wextra)

    argus_runtime_rpath(voice-stt-remote-test)

    set_target_properties(voice-stt-remote-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME voice-stt-remote-test COMMAND voice-stt-remote-test)

    # The LLM cutover suite (F4-5): the voice session streams its replies through
    # the argus-llm internal wire (RemoteVoiceLlm), degrading silently when the
    # server is down.
    add_executable(voice-llm-remote-test tests/unit/voice-llm-remote-test.cc)

    # This suite drives both the LLM and STT fakes.
    target_include_directories(voice-llm-remote-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../argus-llm-client/tests/support
        ${CMAKE_CURRENT_SOURCE_DIR}/../argus-stt-client/tests/support)

    target_link_libraries(voice-llm-remote-test PRIVATE argus::voice-core
        doctest::doctest)

    target_compile_options(voice-llm-remote-test PRIVATE -Wall -Wextra)

    argus_runtime_rpath(voice-llm-remote-test)

    set_target_properties(voice-llm-remote-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME voice-llm-remote-test COMMAND voice-llm-remote-test)
endif()

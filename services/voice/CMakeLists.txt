cmake_minimum_required(VERSION 3.25)
project(argus-voice VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(PROJECT_IS_TOP_LEVEL)
    # Standalone configure: this service owns the whole build, so it adds the
    # packages it links — the leaves it uses directly and the sibling packages
    # that used to arrive through argus-common.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_path(RNNOISE_INCLUDE_DIR NAMES rnnoise.h)
    find_library(RNNOISE_LIBRARY NAMES rnnoise)
    if(NOT TARGET contract::voice)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/voice-contract
                         ${CMAKE_CURRENT_BINARY_DIR}/voice-contract)
    endif()

    if(NOT TARGET argus::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET pkg::response)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/response
                         ${CMAKE_BINARY_DIR}/response)
    endif()
    if(NOT TARGET argus::audio)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/audio
                         ${CMAKE_BINARY_DIR}/audio)
    endif()
    if(NOT TARGET argus::tts-client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts-client
                         ${CMAKE_BINARY_DIR}/tts-client)
    endif()
    if(NOT TARGET argus::llm-client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm-client
                         ${CMAKE_BINARY_DIR}/llm-client)
    endif()
    if(NOT TARGET argus::stt-client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt-client
                         ${CMAKE_BINARY_DIR}/stt-client)
    endif()
    if(NOT TARGET argus::phrase)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/phrase
                         ${CMAKE_BINARY_DIR}/phrase)
    endif()
endif()

if(NOT TARGET argus::client-voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/voice
                     ${CMAKE_BINARY_DIR}/clients/voice EXCLUDE_FROM_ALL)
endif()
# The standard grpc.health.v1 stubs this service answers its health probes
# with; packages/grpc declares them (argus::grpc-health).
if(NOT TARGET argus::grpc-health)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/grpc
                     ${CMAKE_BINARY_DIR}/grpc EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::client-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
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
    DEPENDS
        argus::client-voice
        argus::grpc-health
        argus::client-identity
        argus::audio
        argus::tts-client
        argus::llm-client
        argus::stt-client
        argus::phrase
        contract::voice
        argus::config
    SYSTEM_DEPENDS
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        onnxruntime::onnxruntime
)

# Third-party headers are SYSTEM (AGENTS.md rule 15).
target_include_directories(argus_voice-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/stb)

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

# The voice suites opt back into the default build: this folder is added
# EXCLUDE_FROM_ALL.
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts-client/tests/support)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt-client/tests/support)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm-client/tests/support
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt-client/tests/support)

target_link_libraries(voice-llm-remote-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-llm-remote-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-llm-remote-test)

set_target_properties(voice-llm-remote-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-llm-remote-test COMMAND voice-llm-remote-test)

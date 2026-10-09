cmake_minimum_required(VERSION 3.25)
project(argus-voice VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_path(RNNOISE_INCLUDE_DIR NAMES rnnoise.h)
    find_library(RNNOISE_LIBRARY NAMES rnnoise)
    if(NOT TARGET argus::contracts::voice)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/voice
                         ${CMAKE_BINARY_DIR}/contracts/voice)
    endif()

    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::lib::audio)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/audio
                         ${CMAKE_BINARY_DIR}/audio)
    endif()
    if(NOT TARGET argus::clients::tts)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts
                         ${CMAKE_BINARY_DIR}/clients/tts)
    endif()
    if(NOT TARGET argus::clients::llm)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm
                         ${CMAKE_BINARY_DIR}/clients/llm)
    endif()
    if(NOT TARGET argus::clients::stt)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt
                         ${CMAKE_BINARY_DIR}/clients/stt)
    endif()
    if(NOT TARGET argus::lib::phrase)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/phrase
                         ${CMAKE_BINARY_DIR}/phrase)
    endif()
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
endif()

if(NOT TARGET argus::clients::voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/voice
                     ${CMAKE_BINARY_DIR}/clients/voice EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::lib::grpc-health)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/grpc
                     ${CMAKE_BINARY_DIR}/grpc EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::lib::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                     ${CMAKE_BINARY_DIR}/auth)
endif()

if(NOT COMMAND argus_settings_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/settings
                     ${CMAKE_BINARY_DIR}/contracts/settings)
endif()

find_package(doctest REQUIRED)

set(VOICE_ENGINE_SOURCES
    src/shared/services/vad/vad-service.cc
    src/feature/voice/services/noise/noise-suppression-service.cc
    src/feature/voice/services/reaction/reaction-engine.cc
)

argus_module(NAME voice-core
    SOURCES
        src/feature/voice/call-history.cc
        src/feature/voice/offer-reply.cc
        src/feature/voice/turn-transcript.cc
        src/feature/voice/voice-session-service.cc
        src/feature/voice/voice-engine-seam.cc
        src/feature/voice/voice-rpc-service.cc
        src/feature/health/health-rpc-service.cc
        ${VOICE_ENGINE_SOURCES}
    INCLUDES
        src
    DEPENDS
        argus::clients::voice
        argus::lib::grpc-health
        argus::clients::identity
        argus::lib::auth
        argus::lib::audio
        argus::clients::tts
        argus::clients::llm
        argus::clients::stt
        argus::lib::phrase
        argus::contracts::voice
        argus::lib::config
        argus::voice-config
        argus::lib::http
        argus::lib::runtime
    SYSTEM_DEPENDS
        Drogon::Drogon
        nlohmann_json::nlohmann_json
        onnxruntime::onnxruntime
)

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

add_subdirectory(src/config)
add_subdirectory(src/feature/settings)
add_subdirectory(src/feature/rtc)

argus_service(NAME argus-voice
    MAIN src/main.cc
    MODULES argus::voice-config argus::voice-core argus::voice-settings argus::voice-rtc
    DEPENDS argus::clients::notification
    PORTS 7034 7035)

add_custom_command(TARGET argus-voice POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ARGUS_LIVEKIT_SDK_LIBS} $<TARGET_FILE_DIR:argus-voice>
    VERBATIM)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_executable(voice-session-seam-test tests/unit/voice-session-seam-test.cc)

target_link_libraries(voice-session-seam-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-session-seam-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-session-seam-test)

set_target_properties(voice-session-seam-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-session-seam-test COMMAND voice-session-seam-test)

add_executable(voice-call-history-test tests/unit/voice-call-history-test.cc)

target_compile_definitions(voice-call-history-test PRIVATE
    ARGUS_CALL_PROMPT_ES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/call-prompt-es.txt"
    ARGUS_CALL_PROMPT_EN="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/call-prompt-en.txt"
    ARGUS_CALL_KNOWN_ES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/call-known-es.txt"
    ARGUS_CALL_KNOWN_EN="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/call-known-en.txt")

target_link_libraries(voice-call-history-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-call-history-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-call-history-test)

set_target_properties(voice-call-history-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-call-history-test COMMAND voice-call-history-test)

add_executable(voice-offer-reply-test tests/unit/voice-offer-reply-test.cc)

target_link_libraries(voice-offer-reply-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-offer-reply-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-offer-reply-test)

set_target_properties(voice-offer-reply-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-offer-reply-test COMMAND voice-offer-reply-test)

add_executable(voice-rpc-auth-test tests/unit/voice-rpc-auth-test.cc)

target_link_libraries(voice-rpc-auth-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-rpc-auth-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-rpc-auth-test)

set_target_properties(voice-rpc-auth-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-rpc-auth-test COMMAND voice-rpc-auth-test)

add_executable(voice-reactor-lifetime-test tests/unit/voice-reactor-lifetime-test.cc)

target_link_libraries(voice-reactor-lifetime-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-reactor-lifetime-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-reactor-lifetime-test)

set_target_properties(voice-reactor-lifetime-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-reactor-lifetime-test COMMAND voice-reactor-lifetime-test)

add_executable(voice-tts-remote-test tests/unit/voice-tts-remote-test.cc)

target_include_directories(voice-tts-remote-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts/tests/support)

target_link_libraries(voice-tts-remote-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-tts-remote-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-tts-remote-test)

set_target_properties(voice-tts-remote-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-tts-remote-test COMMAND voice-tts-remote-test)

add_executable(voice-stt-remote-test tests/unit/voice-stt-remote-test.cc)

target_include_directories(voice-stt-remote-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt/tests/support)

target_link_libraries(voice-stt-remote-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-stt-remote-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-stt-remote-test)

set_target_properties(voice-stt-remote-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-stt-remote-test COMMAND voice-stt-remote-test)

add_executable(voice-llm-remote-test tests/unit/voice-llm-remote-test.cc)

target_include_directories(voice-llm-remote-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm/tests/support
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt/tests/support)

target_link_libraries(voice-llm-remote-test PRIVATE argus::voice-core
    doctest::doctest)

target_compile_options(voice-llm-remote-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-llm-remote-test)

set_target_properties(voice-llm-remote-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-llm-remote-test COMMAND voice-llm-remote-test)

add_executable(voice-settings-test tests/unit/voice-settings-test.cc)

target_link_libraries(voice-settings-test PRIVATE argus::voice-core
    argus::voice-settings argus::voice-config doctest::doctest)

target_compile_options(voice-settings-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-settings-test)

set_target_properties(voice-settings-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-settings-test COMMAND voice-settings-test)

add_executable(voice-rtc-wire-test tests/unit/voice-rtc-wire-test.cc)

target_link_libraries(voice-rtc-wire-test PRIVATE argus::voice-rtc-wire
    doctest::doctest)

target_compile_options(voice-rtc-wire-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-rtc-wire-test)

set_target_properties(voice-rtc-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-rtc-wire-test COMMAND voice-rtc-wire-test)

add_executable(voice-playout-gain-test tests/unit/voice-playout-gain-test.cc)

target_link_libraries(voice-playout-gain-test PRIVATE argus::voice-rtc-wire
    doctest::doctest)

target_compile_options(voice-playout-gain-test PRIVATE -Wall -Wextra)

argus_runtime_rpath(voice-playout-gain-test)

set_target_properties(voice-playout-gain-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME voice-playout-gain-test COMMAND voice-playout-gain-test)

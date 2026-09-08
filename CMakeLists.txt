cmake_minimum_required(VERSION 3.25)
project(argus-backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR} ${CMAKE_PREFIX_PATH})

# Marks the root backend project: subprojects test this variable to know
# they are being built as part of the root tree (their root-registered
# ctest suites only exist there).
set(ARGUS_ROOT_PROJECT ON)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/argus-module.cmake)

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(OpenCV CONFIG REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(mdns CONFIG REQUIRED)
find_package(cnats CONFIG REQUIRED)
find_path(RNNOISE_INCLUDE_DIR NAMES rnnoise.h)
find_library(RNNOISE_LIBRARY NAMES rnnoise)

# Before every add_subdirectory: enable_testing() only reaches
# subdirectories added after the call, and service folders added earlier
# (argus-identity) register their root-cttest suites from their own
# CMakeLists.
enable_testing()

add_subdirectory(third_party)

# The foundation and the shared modules, in dependency order: a module may
# only be added after everything it links. Everything below this block is a
# service, discovered rather than listed.
add_subdirectory(argus-common)
add_subdirectory(argus-audio)
add_subdirectory(argus-phrase)
add_subdirectory(argus-tts-client)
add_subdirectory(argus-llm-client)
add_subdirectory(argus-stt-client)
add_subdirectory(argus-cert)
add_subdirectory(argus-room)
add_subdirectory(argus-sqlite)
add_subdirectory(argus-socket)
add_subdirectory(argus-audit)
add_subdirectory(argus-auth)
add_subdirectory(argus-identity)
add_subdirectory(argus-sync)

# argus-voice service (F6-3): before the services discovered below so the
# suites link voice-core with its usage requirements resolved.
add_subdirectory(argus-voice EXCLUDE_FROM_ALL)

# argus-gateway service: built on demand, never as part of the default
# backend build.
add_subdirectory(argus-gateway EXCLUDE_FROM_ALL)

# argus-camera service (Fase 2 pilot): built on demand, never as part of the
# default backend build.
add_subdirectory(argus-camera EXCLUDE_FROM_ALL)

# argus-productivity and argus-notification services (Fase 3 substrate):
# built on demand, never as part of the default backend build.
add_subdirectory(argus-productivity EXCLUDE_FROM_ALL)
add_subdirectory(argus-notification EXCLUDE_FROM_ALL)

# argus-tts service (F4-2): the TTS capacity extracted out of the legacy,
# built on demand, never as part of the default backend build.
add_subdirectory(argus-tts EXCLUDE_FROM_ALL)

# argus-stt service (F4-3): the STT capacity extracted out of the legacy,
# built on demand, never as part of the default backend build.
add_subdirectory(argus-stt EXCLUDE_FROM_ALL)

# argus-vlm service (F4-4): the vision capacity extracted out of the legacy,
# built on demand, never as part of the default backend build.
add_subdirectory(argus-vlm EXCLUDE_FROM_ALL)

# argus-llm service (F4-5): the LLM engine extracted out of the legacy,
# built on demand, never as part of the default backend build.
add_subdirectory(argus-llm EXCLUDE_FROM_ALL)

# argus-memory service (F4-6): the memory stack extracted out of the legacy,
# built on demand, never as part of the default backend build.
add_subdirectory(argus-memory EXCLUDE_FROM_ALL)

# argus-tunnel service (F5-4): the byte-transparent relay pair, built on
# demand, never as part of the default backend build.
add_subdirectory(argus-tunnel EXCLUDE_FROM_ALL)

# Data migration tools now live with the service whose database they write;
# each CLI is built on demand, never as part of the default backend build.
add_subdirectory(argus-identity/tools/migrate-identity EXCLUDE_FROM_ALL)
add_subdirectory(argus-camera/tools/migrate-camera EXCLUDE_FROM_ALL)
add_subdirectory(argus-productivity/tools/migrate-productivity EXCLUDE_FROM_ALL)
add_subdirectory(argus-notification/tools/migrate-notification EXCLUDE_FROM_ALL)

# Vulkan-in-container RADV probe: it reuses ncnn's Vulkan init, which is
# argus-camera's capacity (target argus-vulkan-probe).
add_subdirectory(argus-camera/tools/vulkan-probe EXCLUDE_FROM_ALL)

option(ARGUS_BUILD_LABS "Build backend integration and verification labs" OFF)

if(ARGUS_BUILD_LABS)
    add_subdirectory(labs/audio-probe)
    add_subdirectory(labs/voice-test)
    add_subdirectory(labs/tapo-probe)
    add_subdirectory(labs/llm-bench)
    add_subdirectory(labs/vlm-bench)
    add_subdirectory(labs/tts-probe)
    add_subdirectory(labs/stream-probe)
    add_subdirectory(labs/memory-probe)
    add_subdirectory(labs/intent-probe)
    add_subdirectory(labs/tool-bench)
    add_subdirectory(labs/extract-probe)
    add_subdirectory(labs/reaction-probe)
    add_subdirectory(labs/camera-control)
endif()

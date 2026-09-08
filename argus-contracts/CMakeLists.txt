cmake_minimum_required(VERSION 3.25)
project(argus-contracts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

# Thin per-domain SDK wrappers (AGENTS rule 23): the one wire-handling point
# every consumer shares. Consumers link argus::sdk-voice / argus::sdk-identity
# / argus::sdk-camera.
if(NOT TARGET argus::sdk-voice)
  argus_sdk_module(NAME voice
      PROTO argus/voice/v1/voice.proto
      SOURCES sdk/voice/voice-client.cc
      INCLUDES sdk)
endif()

if(NOT TARGET argus::sdk-identity)
  argus_sdk_module(NAME identity
      PROTO argus/identity/v1/identity.proto
      SOURCES sdk/identity/identity-client.cc
      INCLUDES sdk)
endif()

if(NOT TARGET argus::sdk-camera)
  argus_sdk_module(NAME camera
      PROTO argus/camera/v1/sync.proto
      SOURCES sdk/camera/camera-sync-client.cc
      INCLUDES sdk)
endif()

if(NOT TARGET argus::sdk-health)
  argus_sdk_module(NAME health
      PROTO grpc/health/v1/health.proto)
endif()

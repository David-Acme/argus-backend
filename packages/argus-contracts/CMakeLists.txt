cmake_minimum_required(VERSION 3.25)
project(argus-contracts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

# Standalone configure (top level): the substrate still finds the vendored
# gRPC and would build the cq bridge, whose entry symbol then interposes the
# identically-named implementation inside libgrpc. Empty stand-ins skip it.
if(PROJECT_IS_TOP_LEVEL AND NOT TARGET argus_sdk_grpc_bridge_entry)
  add_library(argus_sdk_grpc_bridge_entry INTERFACE)
  add_library(argus_sdk_grpc_bridge_exit INTERFACE)
endif()

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

# The caller-identity metadata contract suite of the shared client base.
enable_testing()
  find_package(doctest REQUIRED)

  add_executable(sdk-caller-identity-test
  tests/unit/sdk-caller-identity-test.cc)

  # This folder is pulled in EXCLUDE_FROM_ALL (consumers add it for the SDK
  # targets), so the suite has to opt back into the default build to be
  # present when ctest runs it.
  set_target_properties(sdk-caller-identity-test PROPERTIES
  EXCLUDE_FROM_ALL FALSE)

  target_link_libraries(sdk-caller-identity-test PRIVATE argus::sdk-camera
  doctest::doctest)

  target_compile_options(sdk-caller-identity-test PRIVATE -Wall -Wextra)

  add_test(NAME sdk-caller-identity-test COMMAND sdk-caller-identity-test)

  get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                  INTERFACE_INCLUDE_DIRECTORIES)
  if(DOCTEST_INCLUDE_DIRS)
set_target_properties(doctest::doctest PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

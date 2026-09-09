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
# (argus-identity) register their root-ctest suites from their own
# CMakeLists.
enable_testing()

add_subdirectory(third_party)

# The foundation and the shared modules. This list is explicit and ORDERED on
# purpose: a module may only be added after every module it links, so it is the
# one place the dependency order is stated.
set(ARGUS_MODULES
    argus-common
    argus-audio
    argus-phrase
    argus-tts-client
    argus-llm-client
    argus-stt-client
    argus-cert
    argus-room
    argus-sqlite
    argus-socket
    argus-audit
    argus-auth
    argus-identity
    argus-sync)

foreach(argus_module_dir IN LISTS ARGUS_MODULES)
    add_subdirectory(${argus_module_dir})
endforeach()

# argus-contracts is added by whichever consumer needs it first (each guards
# with if(NOT TARGET)), so it is never added here.
# argus-voice is added before the discovered services: their suites link
# voice-core and need its usage requirements resolved.
set(ARGUS_SERVICES_EXPLICIT argus-voice)
set(ARGUS_SERVICES_SKIP argus-contracts ${ARGUS_MODULES})

foreach(argus_service_dir IN LISTS ARGUS_SERVICES_EXPLICIT)
    add_subdirectory(${argus_service_dir} EXCLUDE_FROM_ALL)
endforeach()

# Every other argus-* folder carrying a CMakeLists is a service: discovered,
# not listed, so adding a service means creating its folder. Services are
# built on demand, never as part of the default backend build.
file(GLOB ARGUS_SERVICE_CMAKE CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/argus-*/CMakeLists.txt)

foreach(argus_service_cmake IN LISTS ARGUS_SERVICE_CMAKE)
    get_filename_component(argus_service_dir ${argus_service_cmake} DIRECTORY)
    get_filename_component(argus_service_name ${argus_service_dir} NAME)
    if(NOT argus_service_name IN_LIST ARGUS_SERVICES_SKIP
       AND NOT argus_service_name IN_LIST ARGUS_SERVICES_EXPLICIT)
        add_subdirectory(${argus_service_name} EXCLUDE_FROM_ALL)
    endif()
endforeach()

# Data migration tools live with the service whose database they write, and
# the Vulkan probe with argus-camera whose ncnn Vulkan init it reuses. Same
# discovery rule, same on-demand build.
file(GLOB ARGUS_TOOL_CMAKE CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/argus-*/tools/*/CMakeLists.txt)

foreach(argus_tool_cmake IN LISTS ARGUS_TOOL_CMAKE)
    get_filename_component(argus_tool_dir ${argus_tool_cmake} DIRECTORY)
    file(RELATIVE_PATH argus_tool_rel ${CMAKE_CURRENT_SOURCE_DIR} ${argus_tool_dir})
    add_subdirectory(${argus_tool_rel} EXCLUDE_FROM_ALL)
endforeach()

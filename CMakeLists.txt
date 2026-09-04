cmake_minimum_required(VERSION 3.25)
project(argus-backend VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(CMAKE_PREFIX_PATH ${CMAKE_BINARY_DIR} ${CMAKE_PREFIX_PATH})

find_package(Drogon CONFIG REQUIRED)
find_package(jwt-cpp REQUIRED)
find_package(nlohmann_json REQUIRED)

find_package(OpenCV CONFIG REQUIRED)
find_package(onnxruntime CONFIG REQUIRED)
find_package(tomlplusplus REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(mdns CONFIG REQUIRED)
find_package(qr-code-generator CONFIG REQUIRED)
find_package(cnats CONFIG REQUIRED)
find_path(RNNOISE_INCLUDE_DIR NAMES rnnoise.h)
find_library(RNNOISE_LIBRARY NAMES rnnoise)

add_subdirectory(third_party)

add_subdirectory(src/shared)

enable_testing()

add_subdirectory(${SRC_ROOT}/test/unit ${CMAKE_BINARY_DIR}/test/unit)
add_subdirectory(${SRC_ROOT}/test/e2e ${CMAKE_BINARY_DIR}/test/e2e)

# argus-gateway service: built on demand, never as part of the default
# backend build.
add_subdirectory(argus-gateway EXCLUDE_FROM_ALL)

option(ARGUS_BUILD_LABS "Build backend integration and verification labs" ON)

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
    add_subdirectory(labs/queue-probe)
    add_subdirectory(labs/reaction-probe)
    add_subdirectory(labs/kuzu-probe)
    add_subdirectory(labs/camera-control)
endif()

file(GLOB_RECURSE CONFIG_SRC
    ${SRC_ROOT}/config/*.cc
)

file(GLOB_RECURSE API_SRC
    ${SRC_ROOT}/feature/api/**/*.cc
)

file(GLOB_RECURSE SOCKET_SRC
    ${SRC_ROOT}/feature/socket/**/*.cc
)

file(GLOB_RECURSE FILTER_SRC
    ${SRC_ROOT}/filter/**/*.cc
)

file(GLOB_RECURSE SHARED_SRC
    ${SRC_ROOT}/shared/**/*.cc
)

list(REMOVE_ITEM SHARED_SRC
    ${SRC_ROOT}/shared/wrapper/api-response/api-response.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
    ${SRC_ROOT}/shared/wrapper/nats/nats-bus.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/utils/schema-runner/schema-runner.cc
)

add_executable(${PROJECT_NAME}
    ${SRC_ROOT}/main.cc
)

target_include_directories(${PROJECT_NAME}
    PRIVATE
        ${SRC_ROOT}
        ${CMAKE_BINARY_DIR}/third_party/ncnn/src
)

target_include_directories(${PROJECT_NAME}
    SYSTEM PRIVATE
        ${CMAKE_SOURCE_DIR}/third_party
        ${CMAKE_SOURCE_DIR}/third_party/stb
        ${CMAKE_SOURCE_DIR}/third_party/sherpa-onnx
        ${CMAKE_SOURCE_DIR}/third_party/ncnn/src
)

# toml++ is header-only. Conan's generated imported target can omit its
# include directory in a single-config Release container build, so keep the
# generated package path explicit for every supported configuration.
foreach(_tomlplusplus_include_var
        tomlplusplus_INCLUDE_DIRS_RELEASE
        tomlplusplus_INCLUDE_DIRS_DEBUG
        tomlplusplus_INCLUDE_DIRS)
    if(DEFINED ${_tomlplusplus_include_var})
        target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE
            ${${_tomlplusplus_include_var}})
    endif()
endforeach()

target_sources(${PROJECT_NAME}
    PRIVATE
        ${CONFIG_SRC}
        ${API_SRC}
        ${FILTER_SRC}
        ${SHARED_SRC}
        ${SOCKET_SRC}
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        argus_common
        Drogon::Drogon
        jwt-cpp::jwt-cpp
        nlohmann_json::nlohmann_json

        ${ARGUS_LLAMA_TARGETS}
        opencv::opencv
        onnxruntime::onnxruntime
        tomlplusplus::tomlplusplus
        mdns::mdns
        OpenSSL::SSL
        OpenSSL::Crypto
        ${ARGUS_SQLITE_VEC_TARGET}
        ${ARGUS_NCNN_TARGET}
        ${ARGUS_SHERPA_ONNX_TARGET}
        ${ARGUS_FASTTEXT_TARGET}
        qr-code-generator::qrcodegencpp
        ${CONAN_LIBS}
)

if(RNNOISE_INCLUDE_DIR AND RNNOISE_LIBRARY)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ARGUS_HAS_RNNOISE=1)
  target_include_directories(${PROJECT_NAME} SYSTEM PRIVATE ${RNNOISE_INCLUDE_DIR})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${RNNOISE_LIBRARY})
else()
  target_compile_definitions(${PROJECT_NAME} PRIVATE ARGUS_HAS_RNNOISE=0)
endif()

if(ARGUS_BUILD_INSPIREFACE AND TARGET InspireFace)
  target_link_libraries(${PROJECT_NAME} PRIVATE InspireFace)
endif()

if(ARGUS_SHERPA_ONNX_TARGET AND TARGET ${ARGUS_SHERPA_ONNX_TARGET})
  target_link_libraries(${PROJECT_NAME} PRIVATE ${ARGUS_SHERPA_ONNX_TARGET})
endif()

# Third-party mdns.h is header-only: treat its include dir as SYSTEM so the
# always-on -Wall -Wextra never reports warnings from the vendored header.
if(TARGET mdns::mdns)
  get_target_property(_mdns_include_dirs mdns::mdns INTERFACE_INCLUDE_DIRECTORIES)
  if(_mdns_include_dirs)
    set_target_properties(mdns::mdns PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_mdns_include_dirs}")
  endif()
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# Machine-tuned Release: -march=native is resolved at build time on the
# deployment machine, so binaries stay optimal on any hardware.
target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall
    -Wextra
    $<$<CONFIG:Release>:-O3;-march=native;-flto=auto>
)
target_link_options(${PROJECT_NAME} PRIVATE
    $<$<CONFIG:Release>:-flto=auto>
)

add_custom_command(
    TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/config.toml
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/config.toml
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/models
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/models
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/database
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/database
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/certs
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/certs
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_SOURCE_DIR}/third_party/go2rtc
            $<TARGET_FILE_DIR:${PROJECT_NAME}>/third_party/go2rtc
)

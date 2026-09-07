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
add_subdirectory(src/identity)
add_subdirectory(src/sync)

enable_testing()

add_subdirectory(${SRC_ROOT}/test/unit ${CMAKE_BINARY_DIR}/test/unit)
add_subdirectory(${SRC_ROOT}/test/e2e ${CMAKE_BINARY_DIR}/test/e2e)

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

# Data migration tools: the CLI is built on demand, never as part of the
# default backend build.
add_subdirectory(tools/migrate-identity EXCLUDE_FROM_ALL)
add_subdirectory(tools/migrate-camera EXCLUDE_FROM_ALL)
add_subdirectory(tools/migrate-productivity EXCLUDE_FROM_ALL)
add_subdirectory(tools/migrate-notification EXCLUDE_FROM_ALL)

# Vulkan-in-container RADV probe: built on demand (target argus-vulkan-probe).
add_subdirectory(tools/vulkan-probe EXCLUDE_FROM_ALL)

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

file(GLOB_RECURSE FILTER_SRC
    ${SRC_ROOT}/filter/**/*.cc
)

file(GLOB_RECURSE SHARED_SRC
    ${SRC_ROOT}/shared/**/*.cc
)

file(GLOB_RECURSE IDENTITY_API_SRC
    ${SRC_ROOT}/feature/api/auth/*.cc
    ${SRC_ROOT}/feature/api/invitation/*.cc
    ${SRC_ROOT}/feature/api/pairing/*.cc
    ${SRC_ROOT}/feature/api/user/*.cc
)

list(REMOVE_ITEM API_SRC ${IDENTITY_API_SRC})
list(REMOVE_ITEM FILTER_SRC
    ${SRC_ROOT}/filter/device/device-filter.cc
    ${SRC_ROOT}/filter/jwt/jwt-filter.cc
    ${SRC_ROOT}/filter/role/role-filter.cc
    ${SRC_ROOT}/filter/valid-json/valid-json-filter.cc
)
list(REMOVE_ITEM CONFIG_SRC
    ${SRC_ROOT}/config/app-config.cc
)

list(REMOVE_ITEM SHARED_SRC
    ${SRC_ROOT}/shared/wrapper/api-response/api-response.cc
    ${SRC_ROOT}/shared/wrapper/thread-budget/thread-budget.cc
    ${SRC_ROOT}/shared/wrapper/hardware-profile/hardware-profile.cc
    ${SRC_ROOT}/shared/wrapper/nats/nats-bus.cc
    ${SRC_ROOT}/shared/services/config-service/config-service.cc
    ${SRC_ROOT}/shared/utils/schema-runner/schema-runner.cc
    ${SRC_ROOT}/shared/dtos/socket-emit/socket-emit-dto.cc
    ${SRC_ROOT}/shared/repositories/camera/camera-repository.cc
    ${SRC_ROOT}/shared/repositories/camera-stream/camera-stream-repository.cc
    ${SRC_ROOT}/shared/repositories/zone/zone-repository.cc
    ${SRC_ROOT}/shared/repositories/reminder/reminder-repository.cc
    ${SRC_ROOT}/shared/repositories/reminder-detail/reminder-detail-repository.cc
    ${SRC_ROOT}/shared/repositories/calendar-event/calendar-event-repository.cc
    ${SRC_ROOT}/shared/repositories/calendar-event-share/calendar-event-share-repository.cc
    ${SRC_ROOT}/shared/repositories/project/project-repository.cc
    ${SRC_ROOT}/shared/repositories/project-member/project-member-repository.cc
    ${SRC_ROOT}/shared/repositories/project-task/project-task-repository.cc
    ${SRC_ROOT}/shared/repositories/event/event-repository.cc
    ${SRC_ROOT}/shared/repositories/notification/notification-repository.cc
    ${SRC_ROOT}/shared/schemas/camera/camera-schema.cc
    ${SRC_ROOT}/shared/schemas/camera-stream/camera-stream-schema.cc
    ${SRC_ROOT}/shared/schemas/zone/zone-schema.cc
    ${SRC_ROOT}/shared/schemas/reminder/reminder-schema.cc
    ${SRC_ROOT}/shared/schemas/reminder-detail/reminder-detail-schema.cc
    ${SRC_ROOT}/shared/schemas/calendar-event/calendar-event-schema.cc
    ${SRC_ROOT}/shared/schemas/calendar-event-share/calendar-event-share-schema.cc
    ${SRC_ROOT}/shared/schemas/project/project-schema.cc
    ${SRC_ROOT}/shared/schemas/project-member/project-member-schema.cc
    ${SRC_ROOT}/shared/schemas/project-task/project-task-schema.cc
    ${SRC_ROOT}/shared/schemas/event/event-schema.cc
    ${SRC_ROOT}/shared/schemas/person-event/person-event-schema.cc
    ${SRC_ROOT}/shared/schemas/notification/notification-schema.cc
    ${SRC_ROOT}/shared/repositories/audit-log/audit-log-repository.cc
    ${SRC_ROOT}/shared/repositories/device-credential/device-credential-repository.cc
    ${SRC_ROOT}/shared/repositories/device-login-challenge/device-login-challenge-repository.cc
    ${SRC_ROOT}/shared/repositories/face-embedding/face-embedding-repository.cc
    ${SRC_ROOT}/shared/repositories/person/person-repository.cc
    ${SRC_ROOT}/shared/repositories/portrait-preview-capability/portrait-preview-capability-repository.cc
    ${SRC_ROOT}/shared/repositories/refresh-token/refresh-token-repository.cc
    ${SRC_ROOT}/shared/repositories/stored-file/stored-file-repository.cc
    ${SRC_ROOT}/shared/repositories/user-action-log/user-action-log-repository.cc
    ${SRC_ROOT}/shared/repositories/user-audit-log/user-audit-log-repository.cc
    ${SRC_ROOT}/shared/repositories/user-invitation/user-invitation-repository.cc
    ${SRC_ROOT}/shared/repositories/user-portrait/user-portrait-repository.cc
    ${SRC_ROOT}/shared/repositories/user/user-repository.cc
    ${SRC_ROOT}/shared/repositories/vector-index/vector-index-repository.cc
    ${SRC_ROOT}/shared/schemas/audit-log/audit-log-schema.cc
    ${SRC_ROOT}/shared/schemas/device-credential/device-credential-schema.cc
    ${SRC_ROOT}/shared/schemas/device-login-challenge/device-login-challenge-schema.cc
    ${SRC_ROOT}/shared/schemas/face-embedding/face-embedding-schema.cc
    ${SRC_ROOT}/shared/schemas/person/person-schema.cc
    ${SRC_ROOT}/shared/schemas/portrait-preview-capability/portrait-preview-capability-schema.cc
    ${SRC_ROOT}/shared/schemas/refresh-token/refresh-token-schema.cc
    ${SRC_ROOT}/shared/schemas/stored-file/stored-file-schema.cc
    ${SRC_ROOT}/shared/schemas/user-action-log/user-action-log-schema.cc
    ${SRC_ROOT}/shared/schemas/user-audit-log/user-audit-log-schema.cc
    ${SRC_ROOT}/shared/schemas/user-invitation/user-invitation-schema.cc
    ${SRC_ROOT}/shared/schemas/user-portrait/user-portrait-schema.cc
    ${SRC_ROOT}/shared/schemas/user/user-schema.cc
    ${SRC_ROOT}/shared/services/audit-log/audit-log-service.cc
    ${SRC_ROOT}/shared/services/cert/cert-service.cc
    ${SRC_ROOT}/shared/services/face/face-db.cc
    ${SRC_ROOT}/shared/services/face/face-service.cc
    ${SRC_ROOT}/shared/services/jwt/jwt-service.cc
    ${SRC_ROOT}/shared/services/mdns/mdns-service.cc
    ${SRC_ROOT}/shared/services/room/room-manager.cc
    ${SRC_ROOT}/shared/services/socket/socket-service.cc
    ${SRC_ROOT}/shared/services/sqlite/db-service.cc
    ${SRC_ROOT}/shared/services/sqlite/vec-db.cc
    ${SRC_ROOT}/shared/services/storage/private-portrait-service.cc
    ${SRC_ROOT}/shared/services/storage/s3-storage-service.cc
    ${SRC_ROOT}/shared/services/sync-audit/sync-audit-service.cc
    ${SRC_ROOT}/shared/services/user-action-log/user-action-log-service.cc
    ${SRC_ROOT}/shared/services/user-audit-log/user-audit-log-service.cc
    ${SRC_ROOT}/shared/utils/json-diff/json-diff.cc
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
)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        argus_identity
        argus_sync
        argus_sync_media
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

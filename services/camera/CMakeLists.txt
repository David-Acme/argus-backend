cmake_minimum_required(VERSION 3.25)
project(argus-camera VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(CAMERA_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

    set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NCNN_OPENMP ON CACHE BOOL "" FORCE)
    set(NCNN_AVX2 ON CACHE BOOL "" FORCE)
    set(NCNN_VULKAN ON CACHE BOOL "" FORCE)
    set(NCNN_ENABLE_PIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ncnn
                     ${CMAKE_BINARY_DIR}/third_party/ncnn EXCLUDE_FROM_ALL)
    set(ARGUS_NCNN_TARGET ncnn)
    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()

    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()

    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::lib::text)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/text
                         ${CMAKE_BINARY_DIR}/text)
    endif()
    if(NOT TARGET argus::lib::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::lib::storage)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/storage
                         ${CMAKE_BINARY_DIR}/storage)
    endif()
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::lib::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::lib::audio)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/audio
                         ${CMAKE_BINARY_DIR}/audio)
    endif()
    if(NOT TARGET argus::clients::tts)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts
                         ${CMAKE_BINARY_DIR}/clients/tts)
    endif()
    if(NOT TARGET argus::clients::stt)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt
                         ${CMAKE_BINARY_DIR}/clients/stt)
    endif()
endif()

find_package(doctest REQUIRED)

if(NOT TARGET argus::contracts::camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/camera
                     ${CMAKE_BINARY_DIR}/contracts/camera)
endif()

if(NOT TARGET argus::clients::camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera
                     ${CMAKE_BINARY_DIR}/clients/camera EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::lib::grpc-health)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/grpc
                     ${CMAKE_BINARY_DIR}/grpc EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::camera-actions)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera-actions
                     ${CMAKE_BINARY_DIR}/clients/camera-actions EXCLUDE_FROM_ALL)
endif()

add_subdirectory(src/shared/repositories)
add_subdirectory(src/shared/repositories/action-command)
add_subdirectory(src/shared/repositories/object-event-outbox)
add_subdirectory(src/shared/repositories/change-outbox)

argus_module(NAME camera-rpc
    SOURCES
        src/feature/sync/camera-sync-rpc-service.cc
        src/feature/health/health-rpc-service.cc
    INCLUDES
        src
    DEPENDS
        argus::camera-repositories
        argus::clients::camera
        argus::lib::grpc-health
        argus::contracts::sync)

target_compile_options(argus_camera-rpc PRIVATE -Wall -Wextra)

set(NCNN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ncnn/src)

add_library(camera-core STATIC
    ${CAMERA_SRC_ROOT}/controllers/camera-media-service.cc
    ${CAMERA_SRC_ROOT}/controllers/camera-media-socket.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/controllers/camera-control-controller.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-preset-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-ptz-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-settings-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-talk-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/services/camera-control-feature-service.cc
    ${CAMERA_SRC_ROOT}/shared/services/camera-driver/camera-driver-registry.cc
    ${CAMERA_SRC_ROOT}/shared/services/camera-driver/tapo-driver.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/legacy-stok-transport.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/secure-passthrough-transport.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-api.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-audio.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-client.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-crypto.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-http.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-talk-client.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-ts-muxer.cc
    ${CAMERA_SRC_ROOT}/camera/camera-config.cc
    ${CAMERA_SRC_ROOT}/camera/nats-camera-change-sink.cc
    ${CAMERA_SRC_ROOT}/objects/ncnn-object-detector.cc
    ${CAMERA_SRC_ROOT}/monitor/camera-health-monitor.cc
    ${CAMERA_SRC_ROOT}/monitor/nats-health-event-sink.cc
    ${CAMERA_SRC_ROOT}/operator/camera-operator-service.cc
    ${CAMERA_SRC_ROOT}/operator/event-intelligence.cc
    ${CAMERA_SRC_ROOT}/operator/go2rtc-frame-source.cc
    ${CAMERA_SRC_ROOT}/operator/identity-known-person-matcher.cc
    ${CAMERA_SRC_ROOT}/operator/nats-object-event-sink.cc
    ${CAMERA_SRC_ROOT}/operator/object-event.cc
    ${CAMERA_SRC_ROOT}/operator/operator-config.cc
    ${CAMERA_SRC_ROOT}/operator/zone-provider.cc
    ${CAMERA_SRC_ROOT}/operator/zone-source.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera/controllers/camera-controller.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera/dtos/create-camera-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera/dtos/update-camera-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera/services/camera-feature-service.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/controllers/zone-controller.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/dtos/create-zone-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/dtos/update-zone-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/services/zone-feature-service.cc
    ${CAMERA_SRC_ROOT}/feature/actions/audio-capture.cc
    ${CAMERA_SRC_ROOT}/feature/actions/camera-action-rpc-service.cc
    ${CAMERA_SRC_ROOT}/feature/actions/stt-transcriber.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/camera-source-registrar.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/go2rtc-manager.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/stream-hub.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/snapshot-store.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/upstream-http.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/ws-frame.cc
    ${CAMERA_SRC_ROOT}/shared/services/evidence/evidence-uploader.cc
    ${CAMERA_SRC_ROOT}/shared/services/event-stream/event-stream.cc
)

target_include_directories(camera-core SYSTEM PUBLIC
    ${NCNN_SRC_DIR}
    ${CMAKE_BINARY_DIR}/third_party/ncnn/src)

target_include_directories(camera-core PUBLIC ${CAMERA_SRC_ROOT})

target_link_libraries(camera-core PUBLIC
    argus::camera-repositories
    argus::lib::sqlite
    argus::camera-action-command
    argus::camera-object-event-outbox
    argus::camera-change-outbox
    argus::lib::audio
    argus::clients::tts
    argus::clients::stt
    argus::clients::camera-actions
    sqlite-vec
    opencv::opencv
    ${ARGUS_NCNN_TARGET}
    argus::hardware-profile-gpu
    argus::contracts::sync
    argus::lib::auth
    argus::lib::config
    argus::hardware-profile
    argus::lib::text
    argus::lib::nats
    argus::lib::storage
    argus::lib::runtime
    argus::lib::validation
    argus::lib::http
    OpenSSL::SSL
    OpenSSL::Crypto)
target_link_libraries(camera-core PRIVATE argus::contracts::camera argus::contracts::sync)

target_compile_options(camera-core PRIVATE -Wall -Wextra)

add_executable(argus-camera ${CAMERA_SRC_ROOT}/main.cc)

target_link_libraries(argus-camera PRIVATE camera-core argus_camera-rpc
    argus::lib::auth)

target_compile_options(argus-camera PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set(CAMERA_UNIT_TESTS
    camera-schema-test
    camera-migration-test
    camera-config-test
    camera-controller-test
    camera-operator-test
    camera-health-test
    event-intelligence-test
    camera-source-registrar-test
    camera-change-transaction-test
)

foreach(TEST_NAME IN LISTS CAMERA_UNIT_TESTS)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)

    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)

        target_link_libraries(${TEST_NAME} PRIVATE
    argus::contracts::auth
    argus::contracts::camera
    argus::lib::audio
    argus::lib::auth
    argus::lib::config
    argus::hardware-profile
    argus::lib::text
    argus::lib::nats
    argus::lib::sqlite
    argus::lib::storage
    argus::lib::runtime
    argus::lib::validation
    argus::contracts::sync
    argus::lib::http
    doctest::doctest)

    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

if(PROJECT_IS_TOP_LEVEL AND NOT TARGET camera-migration)
    add_subdirectory(tools/migrate-camera)
endif()

if(PROJECT_IS_TOP_LEVEL AND TARGET ncnn)
    add_subdirectory(tools/vulkan-probe)
endif()
target_link_libraries(camera-schema-test PRIVATE camera-migration)
target_link_libraries(camera-migration-test PRIVATE camera-migration)

target_link_libraries(camera-config-test PRIVATE camera-core)
target_link_libraries(camera-controller-test PRIVATE camera-core)
target_link_libraries(camera-operator-test PRIVATE camera-core)
target_link_libraries(camera-health-test PRIVATE camera-core)
target_link_libraries(event-intelligence-test PRIVATE camera-core)
target_link_libraries(camera-source-registrar-test PRIVATE camera-core)
target_link_libraries(camera-change-transaction-test PRIVATE camera-core)
target_compile_definitions(camera-change-transaction-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

add_executable(camera-talk-cutover-test
    tests/unit/camera-talk-cutover-test.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/controllers/camera-control-controller.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/services/camera-control-feature-service.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-talk-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-ptz-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-preset-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-settings-dto.cc
    ${CAMERA_SRC_ROOT}/shared/services/camera-driver/camera-driver-registry.cc
    ${CAMERA_SRC_ROOT}/shared/services/camera-driver/tapo-driver.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-crypto.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-http.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-audio.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-ts-muxer.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/secure-passthrough-transport.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/legacy-stok-transport.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-client.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-api.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-talk-client.cc)

target_include_directories(camera-talk-cutover-test PRIVATE
    ${CAMERA_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts/tests/support)

target_link_libraries(camera-talk-cutover-test PRIVATE
    argus::camera-repositories
    argus::lib::audio
    argus::lib::config
    argus::lib::text
    argus::lib::runtime
    argus::lib::validation
    argus::lib::http
    argus::clients::tts
    OpenSSL::SSL
    OpenSSL::Crypto argus::contracts::camera
    doctest::doctest)

set_target_properties(camera-talk-cutover-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_options(camera-talk-cutover-test PRIVATE -Wall -Wextra)

add_test(NAME camera-talk-cutover-test COMMAND camera-talk-cutover-test)

add_executable(camera-action-rpc-test
    tests/unit/camera-action-rpc-test.cc
    ${CAMERA_SRC_ROOT}/feature/actions/audio-capture.cc
    ${CAMERA_SRC_ROOT}/feature/actions/camera-action-rpc-service.cc
    ${CAMERA_SRC_ROOT}/feature/actions/stt-transcriber.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/controllers/camera-control-controller.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/services/camera-control-feature-service.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-talk-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-ptz-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-preset-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera-control/dtos/camera-settings-dto.cc
    ${CAMERA_SRC_ROOT}/shared/services/camera-driver/camera-driver-registry.cc
    ${CAMERA_SRC_ROOT}/shared/services/camera-driver/tapo-driver.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-crypto.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-http.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-audio.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-ts-muxer.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/secure-passthrough-transport.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/legacy-stok-transport.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-client.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-api.cc
    ${CAMERA_SRC_ROOT}/shared/services/tapo/tapo-talk-client.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/go2rtc-manager.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/snapshot-store.cc)

target_include_directories(camera-action-rpc-test PRIVATE
    ${CAMERA_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts/tests/support
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt/tests/support)

target_link_libraries(camera-action-rpc-test PRIVATE
    argus::camera-repositories
    argus::camera-action-command
    argus::lib::audio
    argus::lib::config
    argus::lib::text
    argus::lib::sqlite
    argus::lib::runtime
    argus::lib::validation
    argus::lib::http
    argus::clients::tts
    argus::clients::stt
    argus::clients::camera-actions
    OpenSSL::SSL
    OpenSSL::Crypto argus::contracts::camera
    doctest::doctest)

target_compile_options(camera-action-rpc-test PRIVATE -Wall -Wextra)

set_target_properties(camera-action-rpc-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

add_test(NAME camera-action-rpc-test COMMAND camera-action-rpc-test)

add_executable(camera-action-migration-test
    tests/unit/camera-action-migration-test.cc)
target_include_directories(camera-action-migration-test PRIVATE
    ${CAMERA_SRC_ROOT})
target_link_libraries(camera-action-migration-test PRIVATE
    argus::lib::sqlite
    argus::camera-action-command
    doctest::doctest)
target_compile_options(camera-action-migration-test PRIVATE -Wall -Wextra)
set_target_properties(camera-action-migration-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME camera-action-migration-test COMMAND camera-action-migration-test)

add_executable(object-event-outbox-test
    tests/unit/object-event-outbox-test.cc)
target_link_libraries(object-event-outbox-test PRIVATE
    argus::camera-object-event-outbox
    doctest::doctest)
target_compile_definitions(object-event-outbox-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(object-event-outbox-test PRIVATE -Wall -Wextra)
set_target_properties(object-event-outbox-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME object-event-outbox-test COMMAND object-event-outbox-test)

add_executable(object-event-outbox-sink-test
    tests/unit/object-event-outbox-sink-test.cc)
target_link_libraries(object-event-outbox-sink-test PRIVATE camera-core
    doctest::doctest)
target_compile_definitions(object-event-outbox-sink-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(object-event-outbox-sink-test PRIVATE -Wall -Wextra)
set_target_properties(object-event-outbox-sink-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME object-event-outbox-sink-test COMMAND object-event-outbox-sink-test)

add_executable(change-outbox-test
    tests/unit/change-outbox-test.cc)
target_link_libraries(change-outbox-test PRIVATE
    argus::camera-change-outbox
    doctest::doctest)
target_compile_definitions(change-outbox-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(change-outbox-test PRIVATE -Wall -Wextra)
set_target_properties(change-outbox-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME change-outbox-test COMMAND change-outbox-test)

add_executable(change-outbox-sink-test
    tests/unit/change-outbox-sink-test.cc)
target_link_libraries(change-outbox-sink-test PRIVATE camera-core
    doctest::doctest)
target_compile_definitions(change-outbox-sink-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(change-outbox-sink-test PRIVATE -Wall -Wextra)
set_target_properties(change-outbox-sink-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME change-outbox-sink-test COMMAND change-outbox-sink-test)

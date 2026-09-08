cmake_minimum_required(VERSION 3.25)
project(argus-camera VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CAMERA_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(NOT TARGET argus_common)
    # Standalone build: reuse the root project's libraries and third-party
    # sqlite-vec so the camera service links the same objects as the
    # root-tree build.
    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-common
                     ${CMAKE_BINARY_DIR}/argus-common)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/sqlite
                     ${CMAKE_BINARY_DIR}/argus-sqlite)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/audit
                     ${CMAKE_BINARY_DIR}/argus-audit)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/cert
                     ${CMAKE_BINARY_DIR}/argus-cert)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/room
                     ${CMAKE_BINARY_DIR}/argus-room)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/socket
                     ${CMAKE_BINARY_DIR}/argus-socket)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-auth
                     ${CMAKE_BINARY_DIR}/argus-auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-identity
                     ${CMAKE_BINARY_DIR}/argus-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../src/sync
                     ${CMAKE_BINARY_DIR}/argus-sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-audio
                     ${CMAKE_BINARY_DIR}/argus-audio)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-tts-client
                     ${CMAKE_BINARY_DIR}/argus-tts-client)

    enable_language(C)
    add_library(sqlite-vec STATIC
                ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec/sqlite-vec.c)
    target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
    target_include_directories(sqlite-vec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec)
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

    set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(NCNN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NCNN_OPENMP ON CACHE BOOL "" FORCE)
    set(NCNN_AVX2 ON CACHE BOOL "" FORCE)
    set(NCNN_ENABLE_PIC ON CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ncnn
                     ${CMAKE_BINARY_DIR}/third_party/ncnn EXCLUDE_FROM_ALL)
    set(ARGUS_NCNN_TARGET ncnn)
endif()

find_package(doctest REQUIRED)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/argus-module.cmake)

# The camera-domain sync contract (argus.camera.v1.SyncService); consumers
# link argus::sdk-camera.
if(NOT TARGET argus::sdk-camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../argus-contracts
                     ${CMAKE_BINARY_DIR}/argus-contracts EXCLUDE_FROM_ALL)
endif()

# The camera sync tables' repositories and schemas are still compiled by
# argus_sync in the shared tree (they are camera-domain rows the gateway's
# /sync also reads); this include root goes away when that split lands.
argus_module(NAME camera-rpc
    SOURCES
        src/feature/sync/camera-sync-rpc-service.cc
        src/feature/health/health-rpc-service.cc
    INCLUDES
        src
        ../src
    DEPENDS
        argus::sdk-camera
        argus::sdk-health
        argus_common)

target_compile_options(argus_camera-rpc PRIVATE -Wall -Wextra)

# ncnn is THE objects capacity of this service (F2-3): the detector is the
# only AI linkage here — no face/llm/vlm/tts/stt/vad code is compiled.
set(NCNN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/ncnn/src)

add_library(camera-core STATIC
    ${CAMERA_SRC_ROOT}/controllers/camera-media-service.cc
    # Device control: camera-control feature (moved from the legacy in F6-2)
    # plus the camera-driver stack it dispatches through. TTS synthesis is
    # remote-only (TtsClient -> argus-tts wire); no in-process TTS engine is
    # compiled here.
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
    ${CAMERA_SRC_ROOT}/operator/camera-operator-service.cc
    ${CAMERA_SRC_ROOT}/operator/event-intelligence.cc
    ${CAMERA_SRC_ROOT}/operator/go2rtc-frame-source.cc
    ${CAMERA_SRC_ROOT}/operator/nats-object-event-sink.cc
    ${CAMERA_SRC_ROOT}/operator/object-event.cc
    ${CAMERA_SRC_ROOT}/operator/operator-config.cc
    # Camera-domain CRUD, compiled from the shared tree: this service is the
    # sole owner since F6-2 (the legacy binary no longer compiles it).
    ${CAMERA_SRC_ROOT}/feature/api/camera/controllers/camera-controller.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera/dtos/create-camera-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera/dtos/update-camera-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/camera/services/camera-feature-service.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/controllers/zone-controller.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/dtos/create-zone-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/dtos/update-zone-dto.cc
    ${CAMERA_SRC_ROOT}/feature/api/zone/services/zone-feature-service.cc
    # Stream lifecycle: go2rtc supervision and the fMP4 StreamHub relay.
    ${CAMERA_SRC_ROOT}/shared/services/stream/go2rtc-manager.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/stream-hub.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/upstream-http.cc
    ${CAMERA_SRC_ROOT}/shared/services/stream/ws-frame.cc
)

target_include_directories(camera-core SYSTEM PUBLIC
    ${NCNN_SRC_DIR}
    ${CMAKE_BINARY_DIR}/third_party/ncnn/src)

target_include_directories(camera-core PUBLIC ${CAMERA_SRC_ROOT})

target_link_libraries(camera-core PUBLIC argus_sync argus::sqlite
    argus::socket argus::room argus::audio argus::tts-client sqlite-vec
    opencv::opencv ${ARGUS_NCNN_TARGET} argus::hardware-profile-gpu
    OpenSSL::SSL OpenSSL::Crypto)

target_compile_options(camera-core PRIVATE -Wall -Wextra)

add_executable(argus-camera ${CAMERA_SRC_ROOT}/main.cc)

# The filters validate through argus.identity.v1 (argus::sdk-identity rides
# argus::auth); argus_identity arrives transitively through camera-core's
# argus_sync pull, with no link-order meaning left.
target_link_libraries(argus-camera PRIVATE camera-core argus_camera-rpc
    argus::auth)

target_compile_options(argus-camera PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# The camera unit suites are root-project tests: they are part of the root
# ctest run, and camera-migration (tools/) is EXCLUDE_FROM_ALL there.
# Standalone consumer builds never had these suites.
if(ARGUS_ROOT_PROJECT)
    set(CAMERA_UNIT_TESTS
        camera-schema-test
        camera-migration-test
        camera-config-test
        camera-controller-test
        camera-operator-test
        event-intelligence-test
    )

    foreach(TEST_NAME IN LISTS CAMERA_UNIT_TESTS)
        add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)

        # This folder is added EXCLUDE_FROM_ALL, so the suites have to opt
        # back into the default build or ctest registers tests whose
        # binaries never get built.
        set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)

        target_link_libraries(${TEST_NAME} PRIVATE argus_common doctest::doctest)

        target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)

        add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
    endforeach()

    # camera-migration builds only as a dependency of these two.
    target_link_libraries(camera-schema-test PRIVATE camera-migration)
    target_link_libraries(camera-migration-test PRIVATE camera-migration)

    target_link_libraries(camera-config-test PRIVATE camera-core)
    target_link_libraries(camera-controller-test PRIVATE camera-core)
    target_link_libraries(camera-operator-test PRIVATE camera-core)
    target_link_libraries(event-intelligence-test PRIVATE camera-core)

    # The camera-talk cutover: speak() reaches the wire before the device,
    # degrading to the 502 envelope when argus-tts is down. It compiles the
    # control feature and the driver stack directly rather than linking
    # camera-core, which would drag ncnn into a suite that needs none of it.
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

    # The TTS wire's fake server ships with the wire module, so both this
    # suite and argus-voice's can drive the same fake.
    target_include_directories(camera-talk-cutover-test PRIVATE
        ${CAMERA_SRC_ROOT}
        ${CMAKE_CURRENT_SOURCE_DIR}/../argus-tts-client/tests/support)

    target_link_libraries(camera-talk-cutover-test PRIVATE argus_sync
        argus_common argus::audio argus::tts-client OpenSSL::SSL
        OpenSSL::Crypto doctest::doctest)

    set_target_properties(camera-talk-cutover-test PROPERTIES
        EXCLUDE_FROM_ALL FALSE)

    target_compile_options(camera-talk-cutover-test PRIVATE -Wall -Wextra)

    add_test(NAME camera-talk-cutover-test COMMAND camera-talk-cutover-test)
endif()

# Verification labs, built on demand (--target argus-object-bench /
# argus-camera-probe); never part of the default service build.
add_subdirectory(labs/object-bench EXCLUDE_FROM_ALL)
add_subdirectory(labs/camera-probe EXCLUDE_FROM_ALL)

# The camera suites (schema, migration, config) run from src/test/unit so the
# root ctest covers them without a standalone conan gate.

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
if(NOT COMMAND argus_settings_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/settings
                     ${CMAKE_BINARY_DIR}/contracts/settings)
endif()
if(NOT TARGET argus::clients::camera-actions)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera-actions
                     ${CMAKE_BINARY_DIR}/clients/camera-actions EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()

set(NCNN_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ncnn/src)

add_subdirectory(src/config)
add_subdirectory(src/shared/repositories)
add_subdirectory(src/shared/repositories/change-outbox)
add_subdirectory(src/shared/services/change-sink)
add_subdirectory(src/shared/services/tapo)
add_subdirectory(src/shared/services/camera-driver)
add_subdirectory(src/shared/services/stream)
add_subdirectory(src/shared/services/event-stream)

file(GLOB CAMERA_FEATURE_MODULES CONFIGURE_DEPENDS
    "${CAMERA_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS CAMERA_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/app/rpc)

argus_service(NAME argus-camera
    MAIN ${CAMERA_SRC_ROOT}/app/main.cc
    MODULES
        argus::camera-config
        argus::camera-change-sink
        argus::camera-actions
        argus::camera-feature
        argus::camera-control
        argus::camera-zone
        argus::camera-media
        argus::camera-health
        argus::camera-sync
        argus::camera-rpc-server
        argus::camera-monitor
        argus::camera-operator
        argus::camera-settings
        argus::contracts::settings-wire
        argus::lib::auth
        argus::lib::http
        argus::lib::storage
        argus::lib::validation
    PORTS 7026 7036)

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
    camera-stream-test
    camera-change-transaction-test
)

foreach(TEST_NAME IN LISTS CAMERA_UNIT_TESTS)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)

    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)

    target_link_libraries(${TEST_NAME} PRIVATE doctest::doctest)

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

target_link_libraries(camera-config-test PRIVATE
    argus::camera-config
    argus::lib::http)

target_link_libraries(camera-controller-test PRIVATE
    argus::camera-control
    argus::camera-feature
    argus::camera-zone
    argus::camera-media
    argus::camera-monitor
    argus::lib::auth
    argus::lib::errors
    argus::lib::text
    argus::lib::validation
    argus::contracts::auth
    argus::contracts::camera)

target_link_libraries(camera-operator-test PRIVATE
    argus::camera-operator
    argus::camera-stream)

target_link_libraries(camera-health-test PRIVATE
    argus::camera-monitor)

target_link_libraries(event-intelligence-test PRIVATE
    argus::camera-operator)

target_link_libraries(camera-source-registrar-test PRIVATE
    argus::camera-stream)

target_link_libraries(camera-stream-test PRIVATE
    argus::camera-stream)

target_link_libraries(camera-change-transaction-test PRIVATE
    argus::camera-change-sink
    argus::camera-feature
    argus::camera-change-outbox)

target_compile_definitions(camera-change-transaction-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

add_executable(camera-talk-cutover-test
    tests/unit/camera-talk-cutover-test.cc)

target_include_directories(camera-talk-cutover-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts/tests/support)

target_link_libraries(camera-talk-cutover-test PRIVATE
    argus::camera-control
    argus::camera-driver
    argus::lib::errors
    argus::contracts::camera
    doctest::doctest)

set_target_properties(camera-talk-cutover-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_options(camera-talk-cutover-test PRIVATE -Wall -Wextra)

add_test(NAME camera-talk-cutover-test COMMAND camera-talk-cutover-test)

add_executable(tapo-crypto-test
    tests/unit/tapo-crypto-test.cc)

target_link_libraries(tapo-crypto-test PRIVATE
    argus::camera-tapo
    argus::camera-driver
    doctest::doctest)

set_target_properties(tapo-crypto-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_options(tapo-crypto-test PRIVATE -Wall -Wextra)

add_test(NAME tapo-crypto-test COMMAND tapo-crypto-test)

add_executable(tapo-talk-client-test
    tests/unit/tapo-talk-client-test.cc)

target_link_libraries(tapo-talk-client-test PRIVATE
    argus::camera-tapo
    doctest::doctest)

set_target_properties(tapo-talk-client-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

target_compile_options(tapo-talk-client-test PRIVATE -Wall -Wextra)

add_test(NAME tapo-talk-client-test COMMAND tapo-talk-client-test)

add_subdirectory(tools/tapo-probe)

add_executable(camera-sync-auth-test tests/unit/camera-sync-auth-test.cc)
target_link_libraries(camera-sync-auth-test PRIVATE
    argus::camera-sync
    doctest::doctest)
target_compile_options(camera-sync-auth-test PRIVATE -Wall -Wextra)
set_target_properties(camera-sync-auth-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
argus_runtime_rpath(camera-sync-auth-test)
add_test(NAME camera-sync-auth-test COMMAND camera-sync-auth-test)

add_executable(camera-audio-capture-test
    tests/unit/camera-audio-capture-test.cc)
target_link_libraries(camera-audio-capture-test PRIVATE
    argus::camera-actions
    doctest::doctest)
target_compile_options(camera-audio-capture-test PRIVATE -Wall -Wextra)
set_target_properties(camera-audio-capture-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
argus_runtime_rpath(camera-audio-capture-test)
add_test(NAME camera-audio-capture-test COMMAND camera-audio-capture-test)

add_executable(camera-action-rpc-test
    tests/unit/camera-action-rpc-test.cc)

target_include_directories(camera-action-rpc-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/tts/tests/support
    ${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt/tests/support)

target_link_libraries(camera-action-rpc-test PRIVATE
    argus::camera-actions
    argus::camera-stream
    argus::clients::camera-actions
    argus::lib::sqlite
    argus::contracts::camera
    doctest::doctest)

target_compile_options(camera-action-rpc-test PRIVATE -Wall -Wextra)

set_target_properties(camera-action-rpc-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

add_test(NAME camera-action-rpc-test COMMAND camera-action-rpc-test)

add_executable(camera-action-migration-test
    tests/unit/camera-action-migration-test.cc)
target_link_libraries(camera-action-migration-test PRIVATE
    argus::camera-actions
    argus::lib::sqlite
    doctest::doctest)
target_compile_options(camera-action-migration-test PRIVATE -Wall -Wextra)
set_target_properties(camera-action-migration-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME camera-action-migration-test COMMAND camera-action-migration-test)

add_executable(object-event-outbox-test
    tests/unit/object-event-outbox-test.cc)
target_link_libraries(object-event-outbox-test PRIVATE
    argus::camera-operator
    doctest::doctest)
target_compile_definitions(object-event-outbox-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(object-event-outbox-test PRIVATE -Wall -Wextra)
set_target_properties(object-event-outbox-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME object-event-outbox-test COMMAND object-event-outbox-test)

add_executable(object-event-outbox-sink-test
    tests/unit/object-event-outbox-sink-test.cc)
target_link_libraries(object-event-outbox-sink-test PRIVATE
    argus::camera-operator
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
target_link_libraries(change-outbox-sink-test PRIVATE
    argus::camera-change-sink
    argus::camera-change-outbox
    doctest::doctest)
target_compile_definitions(change-outbox-sink-test PRIVATE
    ARGUS_CAMERA_SCHEMA_PATH="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(change-outbox-sink-test PRIVATE -Wall -Wextra)
set_target_properties(change-outbox-sink-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)
add_test(NAME change-outbox-sink-test COMMAND change-outbox-sink-test)

add_executable(camera-settings-test tests/unit/camera-settings-test.cc)
target_link_libraries(camera-settings-test PRIVATE
    argus::camera-settings
    argus::camera-config
    argus::camera-actions
    argus::camera-sync
    argus::camera-stream
    argus::camera-media
    argus::contracts::settings-wire
    doctest::doctest)
target_compile_options(camera-settings-test PRIVATE -Wall -Wextra)
set_target_properties(camera-settings-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
argus_runtime_rpath(camera-settings-test)
add_test(NAME camera-settings-test COMMAND camera-settings-test)

cmake_minimum_required(VERSION 3.25)
project(argus-sync-service VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(SYNC_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    # Standalone configure: this service owns the whole build, so it adds the
    # packages it links — the leaves it uses directly, the engine package
    # (argus_sync, still a package until sub-step d) and the sibling packages
    # that used to arrive through argus-common — plus the third-party
    # sqlite-vec, so this service links the same objects as any other consumer.

    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)

    # argus_identity compiles the face services, whose headers need ncnn; the
    # objects are dropped at link time because nothing in this service
    # references them.
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
    if(NOT TARGET argus::lib::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/cert
                         ${CMAKE_BINARY_DIR}/cert)
    endif()
    if(NOT TARGET argus::contracts::voice)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/voice
                         ${CMAKE_BINARY_DIR}/contracts/voice)
    endif()
    if(NOT TARGET argus::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity
                         ${CMAKE_BINARY_DIR}/identity)
    endif()
    if(NOT TARGET identity-migration)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/identity/tools/migrate-identity
                         ${CMAKE_BINARY_DIR}/identity/tools/migrate-identity)
    endif()
endif()

# The engine still lives in packages/sync until sub-step d moves it here: this
# binary composes it, so the WebSocket protocol code is literally untouched by
# the commit that changes who serves /sync.
if(NOT TARGET argus_sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/sync
                     ${CMAKE_BINARY_DIR}/packages/sync)
endif()

# The control leg's proto (argus.sync.v1.SyncControlService): this service
# serves it, the fleet's callers reach it through argus::clients::sync.
if(NOT TARGET argus::clients::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/sync
                     ${CMAKE_BINARY_DIR}/clients/sync EXCLUDE_FROM_ALL)
endif()

# The three domains whose tables this service pages, and the voice service the
# /sync forwarder relays to: each leg links its own SDK, so the standalone
# configure adds all four.
if(NOT TARGET argus::clients::camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera
                     ${CMAKE_BINARY_DIR}/clients/camera EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::notification)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                     ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::productivity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/productivity
                     ${CMAKE_BINARY_DIR}/clients/productivity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::voice)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/voice
                     ${CMAKE_BINARY_DIR}/clients/voice EXCLUDE_FROM_ALL)
endif()

find_package(doctest REQUIRED)

file(GLOB SYNC_FEATURE_MODULES CONFIGURE_DEPENDS
    "${SYNC_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS SYNC_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/config)
add_subdirectory(src/app/rpc)

argus_service(NAME argus-sync
    MAIN ${SYNC_SRC_ROOT}/app/main.cc
    MODULES argus::sync-config
            argus::sync-transport
            argus::sync-fanout
            argus::sync-control-rpc
    DEPENDS argus::lib::http
            argus::lib::auth
            argus::clients::sync
            Drogon::Drogon
    PORTS 7025 7041)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_subdirectory(tests)

cmake_minimum_required(VERSION 3.25)
project(argus-identity-service VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(IDENTITY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)

    find_package(Drogon CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(jwt-cpp REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    find_package(OpenSSL REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)

    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::lib::audio)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/audio
                         ${CMAKE_BINARY_DIR}/audio)
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
    if(NOT TARGET argus::lib::cert)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/cert
                         ${CMAKE_BINARY_DIR}/cert)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()
    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
    endif()
    if(NOT TARGET argus::contracts::voice)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/voice
                         ${CMAKE_BINARY_DIR}/contracts/voice)
    endif()
    if(NOT TARGET argus::contracts::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/identity
                         ${CMAKE_BINARY_DIR}/contracts/identity)
    endif()

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
    if(NOT TARGET ncnn)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/ncnn
                         ${CMAKE_BINARY_DIR}/third_party/ncnn EXCLUDE_FROM_ALL)
    endif()
    set(ARGUS_NCNN_TARGET ncnn)

    if(NOT TARGET sherpa-onnx-c-api)
        set(json_POPULATED TRUE CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_PYTHON OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_CHECK OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_PORTAUDIO OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_JNI OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_C_API ON CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_WEBSOCKET OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_GPU OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_BINARY OFF CACHE BOOL "" FORCE)
        set(SHERPA_ONNX_ENABLE_TTS OFF CACHE BOOL "" FORCE)
        get_target_property(ARGUS_IDENTITY_ONNX_INCLUDE_DIR
                            onnxruntime::onnxruntime
                            INTERFACE_INCLUDE_DIRECTORIES)
        set(location_onnxruntime_header_dir "${ARGUS_IDENTITY_ONNX_INCLUDE_DIR}")
        set(location_onnxruntime_lib "/__sherpa_use_conan_onnxruntime__")
        set(onnxruntime_lib_files "")
        add_library(onnxruntime INTERFACE IMPORTED)
        target_include_directories(onnxruntime INTERFACE
            "${ARGUS_IDENTITY_ONNX_INCLUDE_DIR}")
        target_link_libraries(onnxruntime INTERFACE onnxruntime::onnxruntime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sherpa-onnx
                         sherpa-onnx-build EXCLUDE_FROM_ALL)
    endif()
endif()

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity)
endif()

if(NOT TARGET argus::clients::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/sync
                     ${CMAKE_BINARY_DIR}/clients/sync)
endif()

if(NOT TARGET argus::clients::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/auth
                     ${CMAKE_BINARY_DIR}/clients/auth)
endif()

find_package(doctest REQUIRED)

add_subdirectory(src/config)
add_subdirectory(src/shared)
add_subdirectory(src/shared/repositories/change-outbox)

file(GLOB IDENTITY_FEATURE_MODULES CONFIGURE_DEPENDS
    "${IDENTITY_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS IDENTITY_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/app/rpc)

argus_service(NAME argus-identity
    MAIN ${IDENTITY_SRC_ROOT}/app/main.cc
    MODULES argus::identity-config
            argus::identity-shared
            argus::identity-user
            argus::identity-invitation
            argus::identity-pairing
            argus::identity-enrollment
            argus::identity-retention
            argus::identity-voiceprint
            argus::identity-privacy
            argus::identity-rpc
    DEPENDS argus::lib::http
            argus::lib::auth
            argus::lib::nats
            argus::lib::cert
            argus::contracts::auth
            argus::clients::sync
            argus::clients::auth
            Drogon::Drogon
            OpenSSL::SSL
            OpenSSL::Crypto
    PORTS 7044 7040)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_subdirectory(tools/migrate-identity)

add_subdirectory(tests)

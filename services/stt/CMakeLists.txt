cmake_minimum_required(VERSION 3.25)
project(argus-stt VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_contracts_substrate()

set(STT_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    if(NOT TARGET argus::lib::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::lib::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::clients::stt)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/stt
                         ${CMAKE_BINARY_DIR}/clients/stt)
    endif()

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
        get_target_property(ARGUS_STT_ONNX_INCLUDE_DIR onnxruntime::onnxruntime
                            INTERFACE_INCLUDE_DIRECTORIES)
        set(location_onnxruntime_header_dir "${ARGUS_STT_ONNX_INCLUDE_DIR}")
        set(location_onnxruntime_lib "/__sherpa_use_conan_onnxruntime__")
        set(onnxruntime_lib_files "")
        add_library(onnxruntime INTERFACE IMPORTED)
        target_include_directories(onnxruntime INTERFACE
            "${ARGUS_STT_ONNX_INCLUDE_DIR}")
        target_link_libraries(onnxruntime INTERFACE onnxruntime::onnxruntime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sherpa-onnx
                         sherpa-onnx-build EXCLUDE_FROM_ALL)
    endif()
endif()

find_package(doctest REQUIRED)

file(GLOB STT_FEATURE_MODULES CONFIGURE_DEPENDS
    "${STT_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS STT_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/config)
add_subdirectory(src/app/rpc)

argus_service(NAME argus-stt
    MAIN ${STT_SRC_ROOT}/app/main.cc
    MODULES
        argus::stt-config
        argus::stt-rpc
        argus::stt
        argus::lib::config
        argus::lib::runtime
        argus::lib::http
        argus::clients::stt
    PORTS 7030)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_custom_command(TARGET argus-stt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models
            $<TARGET_FILE_DIR:argus-stt>/models)

add_executable(stt-wire-test tests/unit/stt-wire-test.cc)

target_include_directories(stt-wire-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_compile_definitions(stt-wire-test PRIVATE
    ARGUS_TEST_STT_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/stt")

target_link_libraries(stt-wire-test PRIVATE argus::stt doctest::doctest)

target_compile_options(stt-wire-test PRIVATE -Wall -Wextra)

set_target_properties(stt-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME stt-wire-test COMMAND stt-wire-test)

add_executable(stt-rpc-test tests/unit/stt-rpc-test.cc)

target_include_directories(stt-rpc-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_link_libraries(stt-rpc-test PRIVATE argus::stt-rpc doctest::doctest)
target_compile_definitions(stt-rpc-test PRIVATE
    ARGUS_TEST_STT_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/stt")
argus_runtime_rpath(stt-rpc-test)

target_compile_options(stt-rpc-test PRIVATE -Wall -Wextra)

set_target_properties(stt-rpc-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME stt-rpc-test COMMAND stt-rpc-test)

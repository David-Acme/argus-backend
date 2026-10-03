cmake_minimum_required(VERSION 3.25)
project(argus-vlm VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_contracts_substrate()

set(VLM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
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
    if(NOT TARGET argus::lib::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET argus::lib::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::clients::vlm)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/vlm
                         ${CMAKE_BINARY_DIR}/clients/vlm)
    endif()

    if(NOT TARGET llama)
        set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
        set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
        set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
        set(LLAMA_BUILD_MTMD ON CACHE BOOL "" FORCE)
        set(MTMD_VIDEO OFF CACHE BOOL "" FORCE)
        set(GGML_NATIVE ON CACHE BOOL "" FORCE)
        set(GGML_OPENMP ON CACHE BOOL "" FORCE)
        set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(GGML_CUDA OFF CACHE BOOL "" FORCE)
        set(GGML_VULKAN OFF CACHE BOOL "" FORCE)

        include(CheckLanguage)
        check_language(CUDA)
        if(CMAKE_CUDA_COMPILER)
            set(GGML_CUDA ON CACHE BOOL "" FORCE)
        else()
            find_package(Vulkan QUIET COMPONENTS glslc)
            find_package(SPIRV-Headers CONFIG QUIET)
            if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE AND SPIRV-Headers_FOUND)
                set(GGML_VULKAN ON CACHE BOOL "" FORCE)
            endif()
        endif()

        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/llama.cpp
                         llama-build EXCLUDE_FROM_ALL)
    endif()
endif()

if(NOT COMMAND argus_settings_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/settings
                     ${CMAKE_BINARY_DIR}/contracts/settings)
endif()

find_package(doctest REQUIRED)

file(GLOB VLM_FEATURE_MODULES CONFIGURE_DEPENDS
    "${VLM_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS VLM_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/config)
add_subdirectory(src/app/rpc)

argus_service(NAME argus-vlm
    MAIN ${VLM_SRC_ROOT}/app/main.cc
    MODULES
        argus::vlm-config
        argus::vlm-rpc
        argus::vlm
        argus::vlm-settings
        argus::lib::config
        argus::hardware-profile
        argus::lib::runtime
        argus::lib::validation
        argus::lib::http
        argus::clients::vlm
        Drogon::Drogon
    PORTS 7031)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_custom_command(TARGET argus-vlm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models
            $<TARGET_FILE_DIR:argus-vlm>/models)

add_executable(vlm-wire-test tests/unit/vlm-wire-test.cc)

set_target_properties(vlm-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

target_include_directories(vlm-wire-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_compile_definitions(vlm-wire-test PRIVATE
    ARGUS_TEST_VLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/vision/lfm2vl-25")

target_link_libraries(vlm-wire-test PRIVATE argus::vlm doctest::doctest)

target_compile_options(vlm-wire-test PRIVATE -Wall -Wextra)

add_test(NAME vlm-wire-test COMMAND vlm-wire-test)

add_executable(vlm-rpc-test tests/unit/vlm-rpc-test.cc)

target_include_directories(vlm-rpc-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)

target_link_libraries(vlm-rpc-test PRIVATE argus::vlm-rpc doctest::doctest)
target_compile_definitions(vlm-rpc-test PRIVATE
    ARGUS_TEST_VLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/vision/lfm2vl-25")
argus_runtime_rpath(vlm-rpc-test)

target_compile_options(vlm-rpc-test PRIVATE -Wall -Wextra)

set_target_properties(vlm-rpc-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME vlm-rpc-test COMMAND vlm-rpc-test)

add_executable(vlm-settings-test tests/unit/vlm-settings-test.cc)

target_link_libraries(vlm-settings-test PRIVATE argus::vlm-rpc argus::vlm-settings
    argus::vlm-config doctest::doctest)
argus_runtime_rpath(vlm-settings-test)

target_compile_options(vlm-settings-test PRIVATE -Wall -Wextra)

set_target_properties(vlm-settings-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME vlm-settings-test COMMAND vlm-settings-test)

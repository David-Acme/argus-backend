cmake_minimum_required(VERSION 3.25)
project(argus-vlm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VLM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure).
    find_package(Drogon CONFIG REQUIRED)
    find_package(OpenCV CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
endif()

find_package(doctest REQUIRED)

# Foundation slice instead of argus_common: argus_common PUBLIC-links ncnn
# (face-era hardware probing) and would drag ncnn symbols into this binary,
# breaking the zero-non-vision-AI-symbols rule (Ruling BO). Same pattern as
# the F4-2 tts-foundation and F4-3 stt-foundation slices.
add_library(vlm-foundation STATIC
    ${SHARED_ROOT}/wrapper/api-response/api-response.cc
    ${SHARED_ROOT}/wrapper/thread-budget/thread-budget.cc
    ${SHARED_ROOT}/wrapper/hardware-profile/hardware-profile.cc
    ${SHARED_ROOT}/services/config-service/config-service.cc
)
target_include_directories(vlm-foundation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(vlm-foundation PRIVATE ARGUS_NO_NCNN_GPU=1)
target_link_libraries(vlm-foundation PUBLIC Drogon::Drogon
    tomlplusplus::tomlplusplus)
target_compile_options(vlm-foundation PRIVATE -Wall -Wextra)

# llama.cpp + libmtmd are THE capacity of this service (Ruling BO): this is
# an AI-heavy service like the legacy, so it links the same vendored engines.
# The mtmd projector must vendor the SAME llama.cpp commit the legacy links
# (third_party/llama.cpp, tag b10305): within the root build the targets are
# the ones third_party already integrates; a standalone build re-adds the
# same submodule directory with the same cache variables.
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

    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp
                     llama-build EXCLUDE_FROM_ALL)
endif()

add_library(vlm-core STATIC
    ${VLM_SRC_ROOT}/controllers/health-controller.cc
    ${VLM_SRC_ROOT}/controllers/vlm-controller.cc
    ${VLM_SRC_ROOT}/server/listener-config.cc
    ${VLM_SRC_ROOT}/vlm/describe-dto.cc
    # Boot plumbing shared with the backend: the centralized response config
    # (envelope/404/405).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/app-config.cc
    # The engine compiled from the shared tree (PORTED pattern), so the
    # legacy binary and this service describe identically.
    ${SHARED_ROOT}/services/vision/vision-service.cc
)

target_include_directories(vlm-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

target_include_directories(vlm-core PUBLIC
    ${VLM_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(vlm-core PUBLIC
    vlm-foundation
    Drogon::Drogon
    opencv::opencv
    nlohmann_json::nlohmann_json
    llama
    mtmd)

target_compile_options(vlm-core PRIVATE -Wall -Wextra)

add_executable(argus-vlm ${VLM_SRC_ROOT}/main.cc)

target_link_libraries(argus-vlm PRIVATE vlm-core)

target_compile_options(argus-vlm PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set_target_properties(argus-vlm PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

add_custom_command(TARGET argus-vlm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../models
            $<TARGET_FILE_DIR:argus-vlm>/models)

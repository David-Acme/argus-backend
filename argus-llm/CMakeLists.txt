cmake_minimum_required(VERSION 3.25)
project(argus-llm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LLM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure).
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
endif()

find_package(doctest REQUIRED)

# llama.cpp is THE capacity of this service (Ruling BS): it links the same
# vendored engine the legacy links (third_party/llama.cpp, tag b10305) — the
# mtmd projector is NOT needed here (chat only, no multimodal). Within the
# root build the target is the one third_party already integrates; a
# standalone build re-adds the same submodule directory with the same cache
# variables.
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
    set(LLAMA_BUILD_MTMD OFF CACHE BOOL "" FORCE)
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

add_library(llm-core STATIC
    ${LLM_SRC_ROOT}/controllers/health-controller.cc
    ${LLM_SRC_ROOT}/controllers/llm-controller.cc
    ${LLM_SRC_ROOT}/server/listener-config.cc
    ${LLM_SRC_ROOT}/llm/chat-dto.cc
    # Boot plumbing shared with the backend: the centralized response config
    # (envelope/404/405).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/app-config.cc
    # The engine compiled from the shared tree (PORTED pattern), so the
    # legacy binary and this service generate identically.
    ${SHARED_ROOT}/services/llm/llm-service.cc
)

target_include_directories(llm-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

target_include_directories(llm-core PUBLIC
    ${LLM_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(llm-core PUBLIC
    argus_common
    argus::hardware-profile
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    llama)

target_compile_options(llm-core PRIVATE -Wall -Wextra)

add_executable(argus-llm ${LLM_SRC_ROOT}/main.cc)

target_link_libraries(argus-llm PRIVATE llm-core)

target_compile_options(argus-llm PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set_target_properties(argus-llm PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

add_custom_command(TARGET argus-llm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../models
            $<TARGET_FILE_DIR:argus-llm>/models)

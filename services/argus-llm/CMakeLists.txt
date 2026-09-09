cmake_minimum_required(VERSION 3.25)
project(argus-llm VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(LLM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${LLM_SRC_ROOT}/shared)

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

    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/llama.cpp
                     llama-build EXCLUDE_FROM_ALL)
endif()

add_library(llm-core STATIC
    ${LLM_SRC_ROOT}/controllers/llm-controller.cc
    ${LLM_SRC_ROOT}/llm/chat-dto.cc
    # The engine compiled from the shared tree (PORTED pattern), so the
    # legacy binary and this service generate identically.
    ${SHARED_ROOT}/services/llm/llm-service.cc
    # The tool-calling loop over that engine: hops, call parsing and the
    # declarations the model reads.
    ${SHARED_ROOT}/services/llm/lfm-adapter.cc
)

target_include_directories(llm-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

target_include_directories(llm-core PUBLIC
    ${LLM_SRC_ROOT})

# memory-core makes this the brain: the tool loop needs the memory tools in
# process, and the memory worker gets LlmService without a wire hop.
target_link_libraries(llm-core PUBLIC
    argus_common
    argus::llm-client
    argus::hardware-profile
    memory-core
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    llama)

target_compile_options(llm-core PRIVATE -Wall -Wextra)

add_executable(argus-llm ${LLM_SRC_ROOT}/main.cc)

# memory-catalog: the hosted stack's replica leg (NATS subscription plus the
# read-only identity/camera snapshot seeds) — the brain owns the retired
# process's role.
target_link_libraries(argus-llm PRIVATE llm-core memory-catalog)

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
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models
            $<TARGET_FILE_DIR:argus-llm>/models)

# Tool-calling accuracy bench. Built with the default target set so it cannot
# rot, but NOT registered with ctest: it needs the real model and takes
# minutes. Run it by hand from the repo root:
#   ./build/dev/services/argus-llm/argus-tool-bench [--limit N] [--verbose]
add_executable(argus-tool-bench tests/bench/tool-calling-bench.cc)

target_link_libraries(argus-tool-bench PRIVATE llm-core)

target_compile_definitions(argus-tool-bench PRIVATE
    ARGUS_BENCH_CASES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tools/check.tsv")

target_compile_options(argus-tool-bench PRIVATE -Wall -Wextra)

set_target_properties(argus-tool-bench PROPERTIES EXCLUDE_FROM_ALL FALSE)

# The LLM wire suite is a root-project test; it opts back into the default
# build because this folder is added EXCLUDE_FROM_ALL.
if(ARGUS_ROOT_PROJECT)
    find_package(doctest REQUIRED)

    add_executable(llm-wire-test tests/unit/llm-wire-test.cc
        ${LLM_SRC_ROOT}/controllers/llm-controller.cc
        ${LLM_SRC_ROOT}/llm/chat-dto.cc
        ${SHARED_ROOT}/services/llm/llm-service.cc
        # The controller drives the loop when tools are registered; the test
        # registers none and exercises the direct paths.
        ${SHARED_ROOT}/services/llm/lfm-adapter.cc)

    target_include_directories(llm-wire-test PRIVATE
        ${CMAKE_SOURCE_DIR}/services/argus-llm/src)

    target_compile_definitions(llm-wire-test PRIVATE
        ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

    target_link_libraries(llm-wire-test PRIVATE argus_common argus::llm-client
        argus::hardware-profile ${ARGUS_LLAMA_TARGETS} doctest::doctest)

    target_compile_options(llm-wire-test PRIVATE -Wall -Wextra)

    set_target_properties(llm-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME llm-wire-test COMMAND llm-wire-test)

    # Parsing and streaming-gate rules, no model: the f8-b4 probes found each
    # of these on live traffic.
    add_executable(llm-tool-parse-test tests/unit/llm-tool-parse-test.cc)

    target_link_libraries(llm-tool-parse-test PRIVATE llm-core doctest::doctest)

    target_compile_options(llm-tool-parse-test PRIVATE -Wall -Wextra)

    set_target_properties(llm-tool-parse-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME llm-tool-parse-test COMMAND llm-tool-parse-test)
endif()

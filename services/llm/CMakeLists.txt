cmake_minimum_required(VERSION 3.25)
project(argus-llm VERSION 1.0.0 LANGUAGES CXX)

# enable_testing() must precede the first add_subdirectory: the flag
# propagates into every subtree, and a directory that misses it gets no
# CTestTestfile — the tests of anything added beneath it then never run.
enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(LLM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${LLM_SRC_ROOT}/shared)

# llama.cpp is THE capacity of this service (Ruling BS): it links the same
# vendored engine the legacy links (third_party/llama.cpp, tag b10305) — the
# mtmd projector is NOT needed here (chat only, no multimodal). Within the
# root build the target is the one third_party already integrates; a
# standalone build re-adds the same submodule directory with the same cache
# variables. Added before the sibling packages so argus-memory's guarded
# llama block is skipped and these cache vars win.
if(NOT TARGET contract::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/auth-contract)
endif()

if(NOT TARGET contract::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync-contract
                     ${CMAKE_CURRENT_BINARY_DIR}/sync-contract)
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
    set(ARGUS_LLAMA_TARGETS llama)
endif()

if(NOT TARGET Drogon::Drogon)
    # Standalone configure: this service owns the whole build, so it adds
    # the packages it links — the leaves it uses directly and the sibling
    # packages that used to arrive through argus-common.
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    if(NOT TARGET argus::config)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/config
                         ${CMAKE_BINARY_DIR}/config)
    endif()
    if(NOT TARGET argus::text)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/text
                         ${CMAKE_BINARY_DIR}/text)
    endif()
    if(NOT TARGET argus::nats)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/nats
                         ${CMAKE_BINARY_DIR}/nats)
    endif()
    if(NOT TARGET argus::runtime)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/runtime
                         ${CMAKE_BINARY_DIR}/runtime)
    endif()
    if(NOT TARGET argus::validation)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/validation
                         ${CMAKE_BINARY_DIR}/validation)
    endif()
    if(NOT TARGET argus::http)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/http
                         ${CMAKE_BINARY_DIR}/http)
    endif()
    if(NOT TARGET argus::phrase)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/phrase
                         ${CMAKE_BINARY_DIR}/phrase)
    endif()
    if(NOT TARGET argus::llm-client)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm-client
                         ${CMAKE_BINARY_DIR}/llm-client)
    endif()
    # sqlite-vec rides its own vendored folder; added before argus-sqlite so
    # its vec repositories see the target.
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)
    if(NOT TARGET argus::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET memory-core)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/memory
                         ${CMAKE_BINARY_DIR}/memory)
    endif()
    if(NOT TARGET argus::intent)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/intent
                         ${CMAKE_BINARY_DIR}/intent)
    endif()
endif()

# The identity and camera contracts the catalog seed reads through the SDK.
if(NOT TARGET argus::client-identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::client-camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera
                     ${CMAKE_BINARY_DIR}/clients/camera EXCLUDE_FROM_ALL)
endif()

find_package(doctest REQUIRED)

add_library(llm-core STATIC
    ${LLM_SRC_ROOT}/controllers/llm-controller.cc
    ${LLM_SRC_ROOT}/llm/chat-dto.cc
    # The engine compiled from the shared tree (PORTED pattern), so the
    # legacy binary and this service generate identically.
    ${SHARED_ROOT}/services/llm/llm-service.cc
    # The tool-calling loop over that engine: hops, call parsing and the
    # declarations the model reads.
    ${SHARED_ROOT}/services/llm/lfm-adapter.cc
    # The runtime the loop drives: the registry services register into, the
    # argument validator and the executor that gates a call on role access.
    # One consumer, so it belongs to this service (D16) and moved here from
    # packages/clients/llm-client with Phase 1 step 9; the vocabulary it runs
    # over stayed in that package, which is the tier argus-memory — the
    # declarer of the descriptors — can also reach.
    ${SHARED_ROOT}/services/tools/tool-registry.cc
    ${SHARED_ROOT}/services/tools/tool-validator.cc
    ${SHARED_ROOT}/services/tools/tool-executor.cc
    # The router's process-lifetime owner: catalog, model and temporal probe.
    ${SHARED_ROOT}/services/llm/intent-gate.cc
)

target_include_directories(llm-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

target_include_directories(llm-core PUBLIC
    ${LLM_SRC_ROOT})

# memory-core makes this the brain: the tool loop needs the memory tools in
# process, and the memory worker gets LlmService without a wire hop.
target_link_libraries(llm-core PUBLIC
    argus::config
    argus::hardware-profile
    argus::phrase
    argus::runtime
    argus::validation
    argus::http
    argus::llm-client
    argus::intent
    memory-core
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    contract::auth
    llama)

target_compile_options(llm-core PRIVATE -Wall -Wextra)

add_library(encounter-consumer STATIC
    ${SHARED_ROOT}/services/encounter-closed/encounter-closed-consumer.cc
)

target_include_directories(encounter-consumer PUBLIC
    ${LLM_SRC_ROOT})

target_link_libraries(encounter-consumer PUBLIC
    argus::text
    argus::nats
    memory-core)

target_compile_options(encounter-consumer PRIVATE -Wall -Wextra)

add_executable(argus-llm ${LLM_SRC_ROOT}/main.cc)

# memory-catalog: the hosted stack's replica leg (NATS subscription plus the
# identity/camera SDK snapshot seeds) — the brain owns the retired
# process's role.
target_link_libraries(argus-llm PRIVATE llm-core memory-catalog
    encounter-consumer
    argus::client-identity argus::client-camera)

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
#   ./build/dev/services/llm/argus-tool-bench [--limit N] [--verbose]
add_executable(argus-tool-bench tests/bench/tool-calling-bench.cc)

target_link_libraries(argus-tool-bench PRIVATE llm-core contract::auth contract::sync)

target_compile_definitions(argus-tool-bench PRIVATE
    ARGUS_BENCH_CASES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tools/check.tsv")

target_compile_options(argus-tool-bench PRIVATE -Wall -Wextra)

set_target_properties(argus-tool-bench PROPERTIES EXCLUDE_FROM_ALL FALSE)

# The LLM wire suite opts back into the default build: this folder is added
# EXCLUDE_FROM_ALL.
find_package(doctest REQUIRED)

add_executable(llm-wire-test tests/unit/llm-wire-test.cc
    ${LLM_SRC_ROOT}/controllers/llm-controller.cc
    ${LLM_SRC_ROOT}/llm/chat-dto.cc
    ${SHARED_ROOT}/services/llm/llm-service.cc
    # The controller drives the loop when tools are registered; the test
    # registers none and exercises the direct paths.
    ${SHARED_ROOT}/services/llm/lfm-adapter.cc
    # lfm-adapter builds an executor over the singleton, so the runtime it
    # drives compiles here too (this target builds its own copies of the
    # service's sources, as it does for the engine).
    ${SHARED_ROOT}/services/tools/tool-registry.cc
    ${SHARED_ROOT}/services/tools/tool-validator.cc
    ${SHARED_ROOT}/services/tools/tool-executor.cc
    # The controller owns the router's gate, so the wire test builds it
    # too: with no model on disk it abstains and the paths stay direct.
    ${SHARED_ROOT}/services/llm/intent-gate.cc)

target_include_directories(llm-wire-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(llm-wire-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

target_link_libraries(llm-wire-test PRIVATE
    argus::config
    argus::hardware-profile
    argus::phrase
    argus::runtime
    argus::validation
    argus::http
    argus::llm-client
    argus::intent
    memory-core
    ${ARGUS_LLAMA_TARGETS}
    contract::auth
    doctest::doctest)

target_compile_options(llm-wire-test PRIVATE -Wall -Wextra)

set_target_properties(llm-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-wire-test COMMAND llm-wire-test)

# Parsing and streaming-gate rules, no model: the f8-b4 probes found each
# of these on live traffic.
add_executable(llm-tool-parse-test tests/unit/llm-tool-parse-test.cc)

target_link_libraries(llm-tool-parse-test PRIVATE llm-core doctest::doctest contract::sync)

target_compile_options(llm-tool-parse-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-parse-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-parse-test COMMAND llm-tool-parse-test)

# The tool runtime's own rules, no model and no database: what the executor
# accepts, what it refuses and what the memory descriptors declare. The suite
# that used to drive memory's tools through this pipeline (memory's
# reminder test) can no longer reach it — the runtime is a service's now — so
# the pipeline is covered here, over the descriptors memory really ships.
add_executable(llm-tool-runtime-test tests/unit/llm-tool-runtime-test.cc)

target_link_libraries(llm-tool-runtime-test PRIVATE llm-core argus::auth
    doctest::doctest)

target_compile_options(llm-tool-runtime-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-runtime-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-runtime-test COMMAND llm-tool-runtime-test)

add_executable(encounter-closed-consumer-test
    tests/unit/encounter-closed-consumer-test.cc)

target_link_libraries(encounter-closed-consumer-test PRIVATE
    encounter-consumer memory-core doctest::doctest)

target_compile_definitions(encounter-closed-consumer-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/memory/database/schema.sql")

target_compile_options(encounter-closed-consumer-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-closed-consumer-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

add_test(NAME encounter-closed-consumer-test
    COMMAND encounter-closed-consumer-test)

# Opt-in live check against a real NATS; skips without ARGUS_NATS_URL.
add_executable(encounter-closed-live-test
    tests/unit/encounter-closed-live-test.cc)

target_link_libraries(encounter-closed-live-test PRIVATE
    encounter-consumer memory-core doctest::doctest)

target_compile_definitions(encounter-closed-live-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/../../packages/memory/database/schema.sql")

target_compile_options(encounter-closed-live-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-closed-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

add_test(NAME encounter-closed-live-test COMMAND encounter-closed-live-test)

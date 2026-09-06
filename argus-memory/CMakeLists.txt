cmake_minimum_required(VERSION 3.25)
project(argus-memory VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MEMORY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../src/shared)

if(NOT TARGET Drogon::Drogon)
    # Standalone build (outside the root project's configure).
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    if(TARGET SQLite::SQLite3)
        set(ARGUS_SQLITE3_TARGET SQLite::SQLite3)
    endif()
endif()

find_package(doctest REQUIRED)

# Foundation slice instead of argus_common: argus_common PUBLIC-links ncnn
# (face-era hardware probing) and would drag ncnn symbols into this binary
# (Ruling BW). Same pattern as the F4-2 tts-foundation, F4-3 stt-foundation,
# F4-4 vlm-foundation and F4-5 llm-foundation slices.
add_library(memory-foundation STATIC
    ${SHARED_ROOT}/wrapper/api-response/api-response.cc
    ${SHARED_ROOT}/wrapper/thread-budget/thread-budget.cc
    ${SHARED_ROOT}/wrapper/hardware-profile/hardware-profile.cc
    ${SHARED_ROOT}/services/config-service/config-service.cc
    ${SHARED_ROOT}/utils/schema-runner/schema-runner.cc
)
target_include_directories(memory-foundation PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(memory-foundation PRIVATE ARGUS_NO_NCNN_GPU=1)
target_link_libraries(memory-foundation PUBLIC Drogon::Drogon
    tomlplusplus::tomlplusplus)
if(TARGET SQLite::SQLite3)
    target_link_libraries(memory-foundation PUBLIC SQLite::SQLite3)
endif()
target_compile_options(memory-foundation PRIVATE -Wall -Wextra)

# sqlite-vec is THE capacity of this service's vector index (Ruling BW):
# vec0 must register on the service's own connection.
if(NOT TARGET sqlite-vec)
    enable_language(C)
    add_library(sqlite-vec STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec/sqlite-vec.c)
    target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
    target_include_directories(sqlite-vec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/sqlite-vec)
endif()

# llama.cpp for the extract model (NuExtract): the same vendored engine the
# legacy links, WITHOUT mtmd (text extraction only).
if(NOT TARGET llama)
    set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
    set(LLAMA_USE_PREBUILT_UI OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_MTMD OFF CACHE BOOL "" FORCE)
    set(GGML_NATIVE ON CACHE BOOL "" FORCE)
    set(GGML_OPENMP ON CACHE BOOL "" FORCE)
    set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GGML_CUDA OFF CACHE BOOL "" FORCE)
    set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../third_party/llama.cpp
                     llama-build EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET onnxruntime::onnxruntime)
    find_package(onnxruntime CONFIG REQUIRED)
endif()
if(NOT TARGET cnats::nats_static)
    find_package(cnats CONFIG REQUIRED)
endif()

add_library(memory-core STATIC
    ${MEMORY_SRC_ROOT}/controllers/health-controller.cc
    ${MEMORY_SRC_ROOT}/controllers/memory-controller.cc
    ${MEMORY_SRC_ROOT}/memory/catalog-replica.cc
    ${MEMORY_SRC_ROOT}/memory/memory-dto.cc
    ${MEMORY_SRC_ROOT}/server/listener-config.cc
    # Boot plumbing shared with the backend: the centralized response config
    # (envelope/404/405).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/config/app-config.cc
    # The memory stack compiled from the shared tree (PORTED pattern), so the
    # legacy binary and this service behave identically.
    ${SHARED_ROOT}/services/memory/memory-service.cc
    ${SHARED_ROOT}/services/memory/sqlite-graph.cc
    ${SHARED_ROOT}/services/memory/entity-resolver.cc
    ${SHARED_ROOT}/services/memory/graph-recall.cc
    ${SHARED_ROOT}/services/memory/memory-formation.cc
    ${SHARED_ROOT}/services/memory/phrase-catalog.cc
    ${SHARED_ROOT}/services/memory/rule-parser.cc
    ${SHARED_ROOT}/services/memory/tool-parser.cc
    ${SHARED_ROOT}/services/embedding/embedding-service.cc
    ${SHARED_ROOT}/services/embedding/unigram-tokenizer.cc
    ${SHARED_ROOT}/services/extract/extraction-service.cc
    ${SHARED_ROOT}/services/extract/lexicon-extractor.cc
    ${SHARED_ROOT}/services/extract/temporal-resolver.cc
    ${SHARED_ROOT}/services/extract/tiered-extractor.cc
    ${SHARED_ROOT}/services/llm/remote/llm-remote.cc
    ${SHARED_ROOT}/services/sqlite/vec-db.cc
    ${SHARED_ROOT}/services/sqlite/db-service.cc
    ${SHARED_ROOT}/services/tools/tool-registry.cc
    ${SHARED_ROOT}/services/tools/tool-validator.cc
    ${SHARED_ROOT}/repositories/memory-graph/memory-graph-repository.cc
    ${SHARED_ROOT}/repositories/vector-index/vector-index-repository.cc
    ${SHARED_ROOT}/utils/json-diff/json-diff.cc
    ${SHARED_ROOT}/utils/text-match/phrase-automaton.cc
    ${SHARED_ROOT}/utils/text-norm/text-norm.cc
    # The NATS event bus wrapper: the catalog replica's change feed (Ruling BX).
    ${SHARED_ROOT}/wrapper/nats/nats-bus.cc
)

target_include_directories(memory-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

target_include_directories(memory-core PUBLIC
    ${MEMORY_SRC_ROOT}
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_link_libraries(memory-core PUBLIC
    memory-foundation
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    llama
    onnxruntime::onnxruntime
    sqlite-vec
    cnats::nats_static)

target_compile_options(memory-core PRIVATE -Wall -Wextra)

add_executable(argus-memory ${MEMORY_SRC_ROOT}/main.cc)

target_link_libraries(argus-memory PRIVATE memory-core)

target_compile_options(argus-memory PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

set_target_properties(argus-memory PROPERTIES
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN")

add_custom_command(TARGET argus-memory POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../models
            $<TARGET_FILE_DIR:argus-memory>/models)

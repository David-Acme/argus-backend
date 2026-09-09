cmake_minimum_required(VERSION 3.25)
project(argus-memory VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(MEMORY_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${MEMORY_SRC_ROOT}/shared)

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

# sqlite-vec is THE capacity of this service's vector index (Ruling BW):
# vec0 must register on the service's own connection.
if(NOT TARGET sqlite-vec)
    enable_language(C)
    add_library(sqlite-vec STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec/sqlite-vec.c)
    target_compile_definitions(sqlite-vec PRIVATE SQLITE_CORE SQLITE_VEC_STATIC)
    target_include_directories(sqlite-vec PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec)
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
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/llama.cpp
                     llama-build EXCLUDE_FROM_ALL)
endif()

if(NOT TARGET onnxruntime::onnxruntime)
    find_package(onnxruntime CONFIG REQUIRED)
endif()
if(NOT TARGET cnats::nats_static)
    find_package(cnats CONFIG REQUIRED)
endif()

add_library(memory-core STATIC
    ${MEMORY_SRC_ROOT}/memory/memory-dto.cc
    ${SHARED_ROOT}/services/memory/memory-service.cc
    ${SHARED_ROOT}/services/memory/memory-tool-descriptors.cc
    ${SHARED_ROOT}/services/memory/sqlite-graph.cc
    ${SHARED_ROOT}/services/memory/entity-resolver.cc
    ${SHARED_ROOT}/services/memory/graph-recall.cc
    ${SHARED_ROOT}/services/memory/memory-formation.cc
    ${SHARED_ROOT}/services/memory/tool-parser.cc
    ${SHARED_ROOT}/services/embedding/embedding-service.cc
    ${SHARED_ROOT}/services/embedding/unigram-tokenizer.cc
    ${SHARED_ROOT}/services/extract/extraction-service.cc
    ${SHARED_ROOT}/services/extract/lexicon-extractor.cc
    ${SHARED_ROOT}/services/extract/temporal-resolver.cc
    ${SHARED_ROOT}/services/extract/tiered-extractor.cc
    ${SHARED_ROOT}/repositories/memory-graph/memory-graph-repository.cc
)

target_include_directories(memory-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

target_include_directories(memory-core PUBLIC ${MEMORY_SRC_ROOT})

target_link_libraries(memory-core PUBLIC
    argus_common
    argus::sqlite
    argus::llm-client
    argus::phrase
    argus::hardware-profile
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    llama
    onnxruntime::onnxruntime
    sqlite-vec)

target_compile_options(memory-core PRIVATE -Wall -Wextra)

# The catalog replica is the one piece that assumes a running process: it
# subscribes to NATS change subjects and seeds from read-only identity/camera
# clients. It stays a separate target so linking the memory package does not
# drag cnats into a consumer that does not want it.
add_library(memory-catalog STATIC ${MEMORY_SRC_ROOT}/memory/catalog-replica.cc)

target_link_libraries(memory-catalog PUBLIC memory-core cnats::nats_static)

target_compile_options(memory-catalog PRIVATE -Wall -Wextra)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

# The memory unit suites are root-project tests; the targets opt back into
# the default build because this folder is added EXCLUDE_FROM_ALL.
if(ARGUS_ROOT_PROJECT)
    # The catalog replica replay (Ruling BX): identity and camera change events
    # land in the replicas and the gazetteer rebuilds; the snapshot fill only
    # fires on empty tables.
    add_executable(memory-replica-test tests/unit/memory-replica-test.cc)

    target_link_libraries(memory-replica-test PRIVATE memory-catalog doctest::doctest)

    target_compile_definitions(memory-replica-test PRIVATE
        ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

    target_compile_options(memory-replica-test PRIVATE -Wall -Wextra)

    set_target_properties(memory-replica-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME memory-replica-test COMMAND memory-replica-test)

    # The queue back-pressure (Ruling BZ): waitForIdle rides the chat port and
    # the bounded queue drops derived-data jobs instead of growing.
    add_executable(memory-backpressure-test tests/unit/memory-backpressure-test.cc)

    target_link_libraries(memory-backpressure-test PRIVATE memory-core
        doctest::doctest)

    target_compile_definitions(memory-backpressure-test PRIVATE
        ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
        ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

    target_compile_options(memory-backpressure-test PRIVATE -Wall -Wextra)

    set_target_properties(memory-backpressure-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME memory-backpressure-test COMMAND memory-backpressure-test)

    # D4: memory.remind writes in the speaking user's own memory, in process,
    # and another user's recall never sees it.
    add_executable(memory-reminder-test tests/unit/memory-reminder-test.cc)

    target_link_libraries(memory-reminder-test PRIVATE memory-core
        doctest::doctest)

    target_compile_definitions(memory-reminder-test PRIVATE
        ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
        ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

    target_compile_options(memory-reminder-test PRIVATE -Wall -Wextra)

    set_target_properties(memory-reminder-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME memory-reminder-test COMMAND memory-reminder-test)

    # F9 Tarea 12: how much of the real memory_save traffic the microsecond
    # lexicon tier carries, which is what decides whether a heavier NER is
    # worth evaluating.
    add_executable(extract-tier-split-test tests/unit/extract-tier-split-test.cc)

    target_link_libraries(extract-tier-split-test PRIVATE memory-core
        doctest::doctest)

    target_compile_definitions(extract-tier-split-test PRIVATE
        ARGUS_TEST_MEMORY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")

    target_compile_options(extract-tier-split-test PRIVATE -Wall -Wextra)

    set_target_properties(extract-tier-split-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

    add_test(NAME extract-tier-split-test COMMAND extract-tier-split-test)

endif()

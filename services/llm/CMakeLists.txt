cmake_minimum_required(VERSION 3.25)
project(argus-llm VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_contracts_substrate()

set(LLM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(PROJECT_IS_TOP_LEVEL)
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
    find_package(onnxruntime CONFIG REQUIRED)
    find_package(cnats CONFIG REQUIRED)
    if(NOT TARGET argus::contracts::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                         ${CMAKE_BINARY_DIR}/contracts/auth)
    endif()
    if(NOT TARGET argus::contracts::sync)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                         ${CMAKE_BINARY_DIR}/contracts/sync)
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
    if(NOT TARGET argus::lib::phrase)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/phrase
                         ${CMAKE_BINARY_DIR}/phrase)
    endif()
    if(NOT TARGET argus::lib::auth)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/auth
                         ${CMAKE_BINARY_DIR}/auth)
    endif()
    if(NOT TARGET argus::clients::llm)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/llm
                         ${CMAKE_BINARY_DIR}/clients/llm)
    endif()
    if(NOT TARGET sqlite-vec)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/sqlite-vec
                         ${CMAKE_BINARY_DIR}/third_party/sqlite-vec)
    endif()
    set(ARGUS_SQLITE_VEC_TARGET sqlite-vec)
    if(NOT TARGET argus::lib::sqlite)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/sqlite
                         ${CMAKE_BINARY_DIR}/sqlite)
    endif()
    if(NOT TARGET fasttext)
        set(FASTTEXT_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/fastText/src)
        add_library(fasttext STATIC
            ${FASTTEXT_SRC}/args.cc
            ${FASTTEXT_SRC}/autotune.cc
            ${FASTTEXT_SRC}/densematrix.cc
            ${FASTTEXT_SRC}/dictionary.cc
            ${FASTTEXT_SRC}/fasttext.cc
            ${FASTTEXT_SRC}/loss.cc
            ${FASTTEXT_SRC}/matrix.cc
            ${FASTTEXT_SRC}/meter.cc
            ${FASTTEXT_SRC}/model.cc
            ${FASTTEXT_SRC}/productquantizer.cc
            ${FASTTEXT_SRC}/quantmatrix.cc
            ${FASTTEXT_SRC}/utils.cc
            ${FASTTEXT_SRC}/vector.cc)
        target_include_directories(fasttext SYSTEM PUBLIC ${FASTTEXT_SRC})
        find_package(Threads REQUIRED)
        target_link_libraries(fasttext PUBLIC Threads::Threads)
        target_compile_options(fasttext PRIVATE -w)
    endif()
    if(NOT TARGET argus::clients::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                         ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::clients::camera)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera
                         ${CMAKE_BINARY_DIR}/clients/camera EXCLUDE_FROM_ALL)
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
    endif()
endif()

if(NOT COMMAND argus_settings_rpc_contract)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/settings
                     ${CMAKE_BINARY_DIR}/contracts/settings)
endif()

find_package(doctest REQUIRED)

file(GLOB LLM_FEATURE_MODULES CONFIGURE_DEPENDS
    "${LLM_SRC_ROOT}/feature/*/CMakeLists.txt")
foreach(module IN LISTS LLM_FEATURE_MODULES)
    get_filename_component(module_dir "${module}" DIRECTORY)
    add_subdirectory("${module_dir}")
endforeach()

add_subdirectory(src/shared)
add_subdirectory(src/config)
add_subdirectory(src/app/rpc)

argus_service(NAME argus-llm
    MAIN ${LLM_SRC_ROOT}/app/main.cc
    MODULES
        argus::llm-config
        argus::llm-rpc
        argus::llm
        argus::llm-settings
        argus::contracts::settings-wire
        argus::llm-shared
        argus::encounter-closed
        argus::memory
        argus::lib::config
        argus::lib::runtime
        argus::lib::text
        argus::lib::nats
        argus::lib::sqlite
        argus::lib::http
        argus::clients::llm
        argus::clients::identity
        argus::clients::camera
        Drogon::Drogon
    PORTS 7032)

get_target_property(DOCTEST_INCLUDE_DIRS doctest::doctest
                    INTERFACE_INCLUDE_DIRECTORIES)
if(DOCTEST_INCLUDE_DIRS)
    set_target_properties(doctest::doctest PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${DOCTEST_INCLUDE_DIRS}")
endif()

add_custom_command(TARGET argus-llm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E create_symlink
            ${CMAKE_CURRENT_SOURCE_DIR}/../../models
            $<TARGET_FILE_DIR:argus-llm>/models)

add_executable(argus-tool-bench tests/bench/tool-calling-bench.cc)

target_link_libraries(argus-tool-bench PRIVATE argus::llm
    argus::contracts::auth argus::contracts::sync)

target_compile_definitions(argus-tool-bench PRIVATE
    ARGUS_BENCH_CASES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tools/check.tsv")

target_compile_options(argus-tool-bench PRIVATE -Wall -Wextra)

set_target_properties(argus-tool-bench PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_executable(llm-wire-test tests/unit/llm-wire-test.cc)

target_compile_definitions(llm-wire-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

target_link_libraries(llm-wire-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-wire-test PRIVATE -Wall -Wextra)

set_target_properties(llm-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-wire-test COMMAND llm-wire-test)

add_executable(llm-rpc-test tests/unit/llm-rpc-test.cc)

target_compile_definitions(llm-rpc-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

target_link_libraries(llm-rpc-test PRIVATE argus::llm-rpc argus::llm
    doctest::doctest)

target_compile_options(llm-rpc-test PRIVATE -Wall -Wextra)

set_target_properties(llm-rpc-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-rpc-test COMMAND llm-rpc-test)

add_executable(llm-settings-test tests/unit/llm-settings-test.cc)

target_link_libraries(llm-settings-test PRIVATE argus::llm-settings argus::llm
    doctest::doctest)

target_compile_options(llm-settings-test PRIVATE -Wall -Wextra)

set_target_properties(llm-settings-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-settings-test COMMAND llm-settings-test)

add_executable(llm-tool-parse-test tests/unit/llm-tool-parse-test.cc)

target_link_libraries(llm-tool-parse-test PRIVATE argus::llm doctest::doctest
    argus::contracts::sync)

target_compile_options(llm-tool-parse-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-parse-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-parse-test COMMAND llm-tool-parse-test)

add_executable(llm-tool-runtime-test tests/unit/llm-tool-runtime-test.cc)

target_link_libraries(llm-tool-runtime-test PRIVATE argus::llm
    doctest::doctest)

target_compile_options(llm-tool-runtime-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-runtime-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-runtime-test COMMAND llm-tool-runtime-test)

add_executable(encounter-closed-consumer-test
    tests/unit/encounter-closed-consumer-test.cc)

target_link_libraries(encounter-closed-consumer-test PRIVATE
    argus::encounter-closed argus::memory doctest::doctest)

target_compile_definitions(encounter-closed-consumer-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(encounter-closed-consumer-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-closed-consumer-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

add_test(NAME encounter-closed-consumer-test
    COMMAND encounter-closed-consumer-test)

add_executable(encounter-closed-live-test
    tests/unit/encounter-closed-live-test.cc)

target_link_libraries(encounter-closed-live-test PRIVATE
    argus::encounter-closed argus::memory doctest::doctest)

target_compile_definitions(encounter-closed-live-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(encounter-closed-live-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-closed-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

add_test(NAME encounter-closed-live-test COMMAND encounter-closed-live-test)

add_executable(memory-replica-test tests/unit/memory-replica-test.cc)

target_link_libraries(memory-replica-test PRIVATE argus::memory
    doctest::doctest)

target_compile_definitions(memory-replica-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(memory-replica-test PRIVATE -Wall -Wextra)

set_target_properties(memory-replica-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-replica-test COMMAND memory-replica-test)

add_executable(memory-backpressure-test tests/unit/memory-backpressure-test.cc)

target_link_libraries(memory-backpressure-test PRIVATE argus::memory
    doctest::doctest)

target_compile_definitions(memory-backpressure-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

target_compile_options(memory-backpressure-test PRIVATE -Wall -Wextra)

set_target_properties(memory-backpressure-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-backpressure-test COMMAND memory-backpressure-test)

add_executable(memory-reminder-test tests/unit/memory-reminder-test.cc)

target_link_libraries(memory-reminder-test PRIVATE argus::memory
    argus::contracts::auth doctest::doctest)

target_compile_definitions(memory-reminder-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

target_compile_options(memory-reminder-test PRIVATE -Wall -Wextra)

set_target_properties(memory-reminder-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-reminder-test COMMAND memory-reminder-test)

add_executable(encounter-inbox-test tests/unit/encounter-inbox-test.cc)

target_link_libraries(encounter-inbox-test PRIVATE argus::memory
    doctest::doctest)

target_compile_definitions(encounter-inbox-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(encounter-inbox-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-inbox-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME encounter-inbox-test COMMAND encounter-inbox-test)

add_executable(extract-tier-split-test tests/unit/extract-tier-split-test.cc)

target_link_libraries(extract-tier-split-test PRIVATE argus::memory
    doctest::doctest)

target_compile_definitions(extract-tier-split-test PRIVATE
    ARGUS_TEST_MEMORY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/memory")

target_compile_options(extract-tier-split-test PRIVATE -Wall -Wextra)

set_target_properties(extract-tier-split-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME extract-tier-split-test COMMAND extract-tier-split-test)

foreach(TEST_NAME IN ITEMS intent-router-test intent-accuracy-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    target_compile_definitions(${TEST_NAME} PRIVATE
        ARGUS_TEST_INTENT_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/intent"
        ARGUS_TEST_INTENT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/intent")
    target_link_libraries(${TEST_NAME} PRIVATE argus::intent doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    set_target_properties(${TEST_NAME} PROPERTIES EXCLUDE_FROM_ALL FALSE)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()

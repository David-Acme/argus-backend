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
    if(NOT TARGET argus::lib::mcp)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/lib/mcp
                         ${CMAKE_BINARY_DIR}/mcp)
    endif()
    if(NOT TARGET argus::clients::mcp)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/mcp
                         ${CMAKE_BINARY_DIR}/clients/mcp)
    endif()
    if(NOT TARGET argus::clients::productivity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/productivity
                         ${CMAKE_BINARY_DIR}/clients/productivity EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::clients::identity)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                         ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
    endif()
    if(NOT TARGET argus::clients::notification)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/notification
                         ${CMAKE_BINARY_DIR}/clients/notification EXCLUDE_FROM_ALL)
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
        argus::pending-intent
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
        argus::clients::mcp
        argus::clients::productivity
        argus::lib::mcp
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

target_compile_definitions(llm-settings-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

target_link_libraries(llm-settings-test PRIVATE argus::llm-settings argus::llm
    doctest::doctest)

target_compile_options(llm-settings-test PRIVATE -Wall -Wextra)

set_target_properties(llm-settings-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-settings-test COMMAND llm-settings-test)

add_executable(llm-app-command-test tests/unit/llm-app-command-test.cc)

target_link_libraries(llm-app-command-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-app-command-test PRIVATE -Wall -Wextra)

set_target_properties(llm-app-command-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-app-command-test COMMAND llm-app-command-test)

add_executable(llm-tool-providers-test tests/unit/llm-tool-providers-test.cc)

target_link_libraries(llm-tool-providers-test PRIVATE argus::llm
    doctest::doctest)

target_compile_options(llm-tool-providers-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-providers-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-providers-test COMMAND llm-tool-providers-test)

add_executable(llm-reply-claims-test tests/unit/llm-reply-claims-test.cc)

target_link_libraries(llm-reply-claims-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-reply-claims-test PRIVATE -Wall -Wextra)

set_target_properties(llm-reply-claims-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-reply-claims-test COMMAND llm-reply-claims-test)

add_executable(llm-plain-turn-test tests/unit/llm-plain-turn-test.cc)

target_link_libraries(llm-plain-turn-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-plain-turn-test PRIVATE -Wall -Wextra)

set_target_properties(llm-plain-turn-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-plain-turn-test COMMAND llm-plain-turn-test)

add_executable(llm-module-command-test tests/unit/llm-module-command-test.cc)

target_link_libraries(llm-module-command-test PRIVATE argus::llm doctest::doctest)

target_compile_definitions(llm-module-command-test PRIVATE
    ARGUS_EVAL_CASES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/eval/cases.jsonl"
    ARGUS_SETTINGS_MODULES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../settings/modules.json")

target_compile_options(llm-module-command-test PRIVATE -Wall -Wextra)

set_target_properties(llm-module-command-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-module-command-test COMMAND llm-module-command-test)

add_executable(llm-turn-flow-test tests/unit/llm-turn-flow-test.cc)

target_link_libraries(llm-turn-flow-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-turn-flow-test PRIVATE -Wall -Wextra)

set_target_properties(llm-turn-flow-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-turn-flow-test COMMAND llm-turn-flow-test)

add_executable(llm-context-selector-test tests/unit/llm-context-selector-test.cc)

target_link_libraries(llm-context-selector-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-context-selector-test PRIVATE -Wall -Wextra)

set_target_properties(llm-context-selector-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-context-selector-test COMMAND llm-context-selector-test)

add_executable(llm-bundle-loader-test tests/unit/llm-bundle-loader-test.cc)

target_link_libraries(llm-bundle-loader-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-bundle-loader-test PRIVATE -Wall -Wextra)

set_target_properties(llm-bundle-loader-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-bundle-loader-test COMMAND llm-bundle-loader-test)

add_executable(llm-turn-engines-test tests/unit/llm-turn-engines-test.cc)

target_compile_definitions(llm-turn-engines-test PRIVATE
    ARGUS_TEST_BUNDLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bundles"
    ARGUS_TEST_LAYA_LABELS="${CMAKE_CURRENT_SOURCE_DIR}/tools/laya-labels.tsv")

target_link_libraries(llm-turn-engines-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-turn-engines-test PRIVATE -Wall -Wextra)

set_target_properties(llm-turn-engines-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-turn-engines-test COMMAND llm-turn-engines-test)

add_executable(llm-sp-tokenizer-test tests/unit/llm-sp-tokenizer-test.cc)

target_compile_definitions(llm-sp-tokenizer-test PRIVATE
    ARGUS_TEST_LAYA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/laya/parity-r1.json"
    ARGUS_TEST_NFC_PAIRS="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/laya/nfc-pairs.json")

target_link_libraries(llm-sp-tokenizer-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-sp-tokenizer-test PRIVATE -Wall -Wextra)

set_target_properties(llm-sp-tokenizer-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-sp-tokenizer-test COMMAND llm-sp-tokenizer-test)

add_executable(llm-gliner-decoder-test tests/unit/llm-gliner-decoder-test.cc)

target_link_libraries(llm-gliner-decoder-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-gliner-decoder-test PRIVATE -Wall -Wextra)

set_target_properties(llm-gliner-decoder-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-gliner-decoder-test COMMAND llm-gliner-decoder-test)

add_executable(llm-spoken-intent-test tests/unit/llm-spoken-intent-test.cc)

target_link_libraries(llm-spoken-intent-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-spoken-intent-test PRIVATE -Wall -Wextra)

set_target_properties(llm-spoken-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-spoken-intent-test COMMAND llm-spoken-intent-test)

add_executable(llm-time-arguments-test tests/unit/llm-time-arguments-test.cc)

target_link_libraries(llm-time-arguments-test PRIVATE argus::llm doctest::doctest)

target_compile_options(llm-time-arguments-test PRIVATE -Wall -Wextra)

set_target_properties(llm-time-arguments-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-time-arguments-test COMMAND llm-time-arguments-test)

add_executable(llm-pending-intent-test tests/unit/llm-pending-intent-test.cc)

target_link_libraries(llm-pending-intent-test PRIVATE argus::llm argus::pending-intent
    doctest::doctest)

target_compile_definitions(llm-pending-intent-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(llm-pending-intent-test PRIVATE -Wall -Wextra)

set_target_properties(llm-pending-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-pending-intent-test COMMAND llm-pending-intent-test)

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
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory"
    ARGUS_TEST_EXTRACT_MODEL="${CMAKE_CURRENT_SOURCE_DIR}/../../models/extract/NuExtract-1.5-tiny-Q4_K_M.gguf")

target_compile_options(memory-reminder-test PRIVATE -Wall -Wextra)

set_target_properties(memory-reminder-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-reminder-test COMMAND memory-reminder-test)

add_executable(notification-reminder-calls-test tests/unit/notification-reminder-calls-test.cc)

target_link_libraries(notification-reminder-calls-test PRIVATE argus::memory doctest::doctest)

target_compile_options(notification-reminder-calls-test PRIVATE -Wall -Wextra)

set_target_properties(notification-reminder-calls-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME notification-reminder-calls-test COMMAND notification-reminder-calls-test)

add_executable(reminder-readback-test tests/unit/reminder-readback-test.cc)

target_link_libraries(reminder-readback-test PRIVATE argus::memory doctest::doctest)

target_compile_options(reminder-readback-test PRIVATE -Wall -Wextra)

set_target_properties(reminder-readback-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME reminder-readback-test COMMAND reminder-readback-test)

add_executable(call-time-test tests/unit/call-time-test.cc)

target_link_libraries(call-time-test PRIVATE argus::memory doctest::doctest)

target_compile_options(call-time-test PRIVATE -Wall -Wextra)

set_target_properties(call-time-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME call-time-test COMMAND call-time-test)

add_executable(encounter-inbox-test tests/unit/encounter-inbox-test.cc)

target_link_libraries(encounter-inbox-test PRIVATE argus::memory
    doctest::doctest)

target_compile_definitions(encounter-inbox-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(encounter-inbox-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-inbox-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME encounter-inbox-test COMMAND encounter-inbox-test)

add_executable(memory-forget-test tests/unit/memory-forget-test.cc)

target_link_libraries(memory-forget-test PRIVATE argus::memory
    doctest::doctest)

target_compile_definitions(memory-forget-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(memory-forget-test PRIVATE -Wall -Wextra)

set_target_properties(memory-forget-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-forget-test COMMAND memory-forget-test)

add_executable(memory-vec-layout-test tests/unit/memory-vec-layout-test.cc)

target_link_libraries(memory-vec-layout-test PRIVATE argus::memory
    argus::contracts::auth doctest::doctest)

target_compile_definitions(memory-vec-layout-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

target_compile_options(memory-vec-layout-test PRIVATE -Wall -Wextra)

set_target_properties(memory-vec-layout-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-vec-layout-test COMMAND memory-vec-layout-test)

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

add_subdirectory(tests/eval)

add_executable(llm-components-test tests/unit/llm-components-test.cc)

target_link_libraries(llm-components-test PRIVATE argus::llm-settings argus::llm-config
    argus::contracts::settings-wire doctest::doctest)
argus_runtime_rpath(llm-components-test)

target_compile_options(llm-components-test PRIVATE -Wall -Wextra)

set_target_properties(llm-components-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-components-test COMMAND llm-components-test)

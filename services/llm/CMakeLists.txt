cmake_minimum_required(VERSION 3.25)
project(argus-llm VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

set(LLM_SRC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(SHARED_ROOT ${LLM_SRC_ROOT}/shared)

if(NOT TARGET argus::contracts::auth)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/auth
                     ${CMAKE_BINARY_DIR}/contracts/auth)
endif()

if(NOT TARGET argus::contracts::sync)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/contracts/sync
                     ${CMAKE_BINARY_DIR}/contracts/sync)
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
    find_package(Drogon CONFIG REQUIRED)
    find_package(tomlplusplus REQUIRED)
    find_package(nlohmann_json REQUIRED)
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
    if(NOT TARGET memory-core)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/memory
                         ${CMAKE_BINARY_DIR}/memory)
    endif()
    if(NOT TARGET argus::intent)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/intent
                         ${CMAKE_BINARY_DIR}/intent)
    endif()
endif()

if(NOT TARGET argus::clients::identity)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/identity
                     ${CMAKE_BINARY_DIR}/clients/identity EXCLUDE_FROM_ALL)
endif()
if(NOT TARGET argus::clients::camera)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/camera
                     ${CMAKE_BINARY_DIR}/clients/camera EXCLUDE_FROM_ALL)
endif()

find_package(doctest REQUIRED)

add_library(llm-core STATIC
    ${LLM_SRC_ROOT}/controllers/llm-controller.cc
    ${LLM_SRC_ROOT}/llm/chat-dto.cc
    ${SHARED_ROOT}/services/llm/llm-service.cc
    ${SHARED_ROOT}/services/llm/lfm-adapter.cc
    ${SHARED_ROOT}/services/tools/tool-registry.cc
    ${SHARED_ROOT}/services/tools/tool-validator.cc
    ${SHARED_ROOT}/services/tools/tool-executor.cc
    ${SHARED_ROOT}/services/llm/intent-gate.cc
)

target_include_directories(llm-core SYSTEM PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)

target_include_directories(llm-core PUBLIC
    ${LLM_SRC_ROOT})

target_link_libraries(llm-core PUBLIC
    argus::lib::config
    argus::lib::auth
    argus::hardware-profile
    argus::lib::phrase
    argus::lib::runtime
    argus::lib::validation
    argus::lib::http
    argus::clients::llm
    argus::intent
    memory-core
    Drogon::Drogon
    nlohmann_json::nlohmann_json
    argus::contracts::auth
    llama)

target_compile_options(llm-core PRIVATE -Wall -Wextra)

add_library(encounter-consumer STATIC
    ${SHARED_ROOT}/services/encounter-closed/encounter-closed-consumer.cc
)

target_include_directories(encounter-consumer PUBLIC
    ${LLM_SRC_ROOT})

target_link_libraries(encounter-consumer PUBLIC
    argus::lib::text
    argus::lib::nats
    memory-core)

target_compile_options(encounter-consumer PRIVATE -Wall -Wextra)

add_executable(argus-llm ${LLM_SRC_ROOT}/main.cc)

target_link_libraries(argus-llm PRIVATE llm-core memory-catalog
    encounter-consumer
    argus::clients::identity argus::clients::camera)

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

add_executable(argus-tool-bench tests/bench/tool-calling-bench.cc)

target_link_libraries(argus-tool-bench PRIVATE llm-core argus::contracts::auth argus::contracts::sync)

target_compile_definitions(argus-tool-bench PRIVATE
    ARGUS_BENCH_CASES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/tools/check.tsv")

target_compile_options(argus-tool-bench PRIVATE -Wall -Wextra)

set_target_properties(argus-tool-bench PROPERTIES EXCLUDE_FROM_ALL FALSE)

find_package(doctest REQUIRED)

add_executable(llm-wire-test tests/unit/llm-wire-test.cc
    ${LLM_SRC_ROOT}/controllers/llm-controller.cc
    ${LLM_SRC_ROOT}/llm/chat-dto.cc
    ${SHARED_ROOT}/services/llm/llm-service.cc
    ${SHARED_ROOT}/services/llm/lfm-adapter.cc
    ${SHARED_ROOT}/services/tools/tool-registry.cc
    ${SHARED_ROOT}/services/tools/tool-validator.cc
    ${SHARED_ROOT}/services/tools/tool-executor.cc
    ${SHARED_ROOT}/services/llm/intent-gate.cc)

target_include_directories(llm-wire-test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_definitions(llm-wire-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

target_link_libraries(llm-wire-test PRIVATE
    argus::lib::config
    argus::lib::auth
    argus::hardware-profile
    argus::lib::phrase
    argus::lib::runtime
    argus::lib::validation
    argus::lib::http
    argus::clients::llm
    argus::intent
    memory-core
    ${ARGUS_LLAMA_TARGETS}
    argus::contracts::auth
    doctest::doctest)

target_compile_options(llm-wire-test PRIVATE -Wall -Wextra)

set_target_properties(llm-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-wire-test COMMAND llm-wire-test)

add_executable(llm-tool-parse-test tests/unit/llm-tool-parse-test.cc)

target_link_libraries(llm-tool-parse-test PRIVATE llm-core doctest::doctest argus::contracts::sync)

target_compile_options(llm-tool-parse-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-parse-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-parse-test COMMAND llm-tool-parse-test)

add_executable(llm-tool-runtime-test tests/unit/llm-tool-runtime-test.cc)

target_link_libraries(llm-tool-runtime-test PRIVATE llm-core argus::lib::auth
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

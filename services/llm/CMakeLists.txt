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
    if(NOT TARGET argus::clients::guard)
        add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../packages/clients/guard
                         ${CMAKE_BINARY_DIR}/clients/guard EXCLUDE_FROM_ALL)
    endif()

    argus_vendored_llama()
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
        argus::duress
        argus::llm-settings
        argus::contracts::settings-wire
        argus::llm-shared
        argus::encounter-closed
        argus::memory
        argus::messenger
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

target_link_libraries(llm-wire-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-wire-test PRIVATE -Wall -Wextra)

set_target_properties(llm-wire-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-wire-test COMMAND llm-wire-test)

add_executable(llm-rpc-test tests/unit/llm-rpc-test.cc)

target_compile_definitions(llm-rpc-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

target_link_libraries(llm-rpc-test PRIVATE argus::llm-rpc argus::llm
    argus::lib::grpc-test-support argus::lib::doctest-main)

target_compile_options(llm-rpc-test PRIVATE -Wall -Wextra)

set_target_properties(llm-rpc-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-rpc-test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/internal-tls-test.sh
                 $<TARGET_FILE:llm-rpc-test> 30)
set_tests_properties(llm-rpc-test PROPERTIES SKIP_RETURN_CODE 77)

add_executable(llm-settings-test tests/unit/llm-settings-test.cc)

target_compile_definitions(llm-settings-test PRIVATE
    ARGUS_TEST_LLM_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/llm")

target_link_libraries(llm-settings-test PRIVATE argus::llm-settings argus::llm
    argus::lib::doctest-main)

target_compile_options(llm-settings-test PRIVATE -Wall -Wextra)

set_target_properties(llm-settings-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-settings-test COMMAND llm-settings-test)

add_executable(llm-app-command-test tests/unit/llm-app-command-test.cc)

target_link_libraries(llm-app-command-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-app-command-test PRIVATE -Wall -Wextra)

set_target_properties(llm-app-command-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-app-command-test COMMAND llm-app-command-test)

add_executable(llm-sight-command-test tests/unit/llm-sight-command-test.cc)

target_link_libraries(llm-sight-command-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-sight-command-test PRIVATE -Wall -Wextra)

set_target_properties(llm-sight-command-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-sight-command-test COMMAND llm-sight-command-test)

add_executable(llm-whereabouts-command-test tests/unit/llm-whereabouts-command-test.cc)

target_link_libraries(llm-whereabouts-command-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-whereabouts-command-test PRIVATE -Wall -Wextra)

set_target_properties(llm-whereabouts-command-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-whereabouts-command-test COMMAND llm-whereabouts-command-test)

add_executable(llm-tool-providers-test tests/unit/llm-tool-providers-test.cc)

target_link_libraries(llm-tool-providers-test PRIVATE argus::llm
    argus::lib::doctest-main)

target_compile_options(llm-tool-providers-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-providers-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-providers-test COMMAND llm-tool-providers-test)

add_executable(llm-reply-claims-test tests/unit/llm-reply-claims-test.cc)

target_link_libraries(llm-reply-claims-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-reply-claims-test PRIVATE -Wall -Wextra)

set_target_properties(llm-reply-claims-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-reply-claims-test COMMAND llm-reply-claims-test)

add_executable(llm-plain-turn-test tests/unit/llm-plain-turn-test.cc)

target_link_libraries(llm-plain-turn-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-plain-turn-test PRIVATE -Wall -Wextra)

set_target_properties(llm-plain-turn-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-plain-turn-test COMMAND llm-plain-turn-test)

add_executable(llm-module-command-test tests/unit/llm-module-command-test.cc)

target_link_libraries(llm-module-command-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_definitions(llm-module-command-test PRIVATE
    ARGUS_EVAL_CASES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/eval/cases.jsonl"
    ARGUS_SETTINGS_MODULES_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../settings/modules.json")

target_compile_options(llm-module-command-test PRIVATE -Wall -Wextra)

set_target_properties(llm-module-command-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-module-command-test COMMAND llm-module-command-test)

add_executable(llm-messenger-vocabulary-test tests/unit/llm-messenger-vocabulary-test.cc)

target_link_libraries(llm-messenger-vocabulary-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_options(llm-messenger-vocabulary-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-vocabulary-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-vocabulary-test COMMAND llm-messenger-vocabulary-test)

add_executable(llm-messenger-command-test tests/unit/llm-messenger-command-test.cc)

target_link_libraries(llm-messenger-command-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_options(llm-messenger-command-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-command-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-command-test COMMAND llm-messenger-command-test)

add_executable(llm-messenger-recipient-test tests/unit/llm-messenger-recipient-test.cc)

target_link_libraries(llm-messenger-recipient-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_options(llm-messenger-recipient-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-recipient-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-recipient-test COMMAND llm-messenger-recipient-test)

add_executable(llm-messenger-thread-test tests/unit/llm-messenger-thread-test.cc)

target_link_libraries(llm-messenger-thread-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_definitions(llm-messenger-thread-test PRIVATE
    ARGUS_TEST_LLM_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(llm-messenger-thread-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-thread-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-thread-test COMMAND llm-messenger-thread-test)

add_executable(llm-messenger-lines-test tests/unit/llm-messenger-lines-test.cc)

target_link_libraries(llm-messenger-lines-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_options(llm-messenger-lines-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-lines-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-lines-test COMMAND llm-messenger-lines-test)

add_executable(llm-messenger-service-test tests/unit/llm-messenger-service-test.cc)

target_link_libraries(llm-messenger-service-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_definitions(llm-messenger-service-test PRIVATE
    ARGUS_TEST_LLM_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(llm-messenger-service-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-service-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-service-test COMMAND llm-messenger-service-test)

add_executable(llm-messenger-roster-test tests/unit/llm-messenger-roster-test.cc)

target_link_libraries(llm-messenger-roster-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_options(llm-messenger-roster-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-roster-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-roster-test COMMAND llm-messenger-roster-test)

add_executable(llm-messenger-wiring-test tests/unit/llm-messenger-wiring-test.cc)

target_link_libraries(llm-messenger-wiring-test PRIVATE argus::messenger argus::lib::doctest-main)

target_compile_options(llm-messenger-wiring-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-wiring-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-wiring-test COMMAND llm-messenger-wiring-test)

add_executable(llm-messenger-concurrency-test tests/unit/llm-messenger-concurrency-test.cc)

target_link_libraries(llm-messenger-concurrency-test PRIVATE argus::messenger argus::memory argus::lib::doctest-main)

target_compile_definitions(llm-messenger-concurrency-test PRIVATE
    ARGUS_TEST_LLM_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(llm-messenger-concurrency-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-concurrency-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-concurrency-test COMMAND llm-messenger-concurrency-test)

add_executable(llm-messenger-tool-test tests/unit/llm-messenger-tool-test.cc)

target_link_libraries(llm-messenger-tool-test PRIVATE argus::llm argus::messenger argus::lib::doctest-main)

target_compile_definitions(llm-messenger-tool-test PRIVATE
    ARGUS_TEST_LLM_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(llm-messenger-tool-test PRIVATE -Wall -Wextra)

set_target_properties(llm-messenger-tool-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-messenger-tool-test COMMAND llm-messenger-tool-test)

add_executable(llm-duress-screen-test tests/unit/llm-duress-screen-test.cc)
target_link_libraries(llm-duress-screen-test PRIVATE argus::duress argus::llm argus::memory argus::lib::doctest-main)
target_compile_definitions(llm-duress-screen-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(llm-duress-screen-test PRIVATE -Wall -Wextra)
set_target_properties(llm-duress-screen-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-duress-screen-test COMMAND llm-duress-screen-test)

add_executable(llm-turn-flow-test tests/unit/llm-turn-flow-test.cc)

target_link_libraries(llm-turn-flow-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-turn-flow-test PRIVATE -Wall -Wextra)

set_target_properties(llm-turn-flow-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-turn-flow-test COMMAND llm-turn-flow-test)

add_executable(llm-speech-acts-test tests/unit/llm-speech-acts-test.cc)

target_link_libraries(llm-speech-acts-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-speech-acts-test PRIVATE -Wall -Wextra)

set_target_properties(llm-speech-acts-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-speech-acts-test COMMAND llm-speech-acts-test)

set_tests_properties(llm-speech-acts-test PROPERTIES
    ENVIRONMENT "ARGUS_TEST_SPEECH_EXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/eval/speech-examples.jsonl;ARGUS_TEST_CALL_PROMPT_ES=${CMAKE_CURRENT_SOURCE_DIR}/../voice/tests/fixtures/call-prompt-es.txt;ARGUS_TEST_CALL_PROMPT_EN=${CMAKE_CURRENT_SOURCE_DIR}/../voice/tests/fixtures/call-prompt-en.txt")

add_executable(llm-speech-cues-test tests/unit/llm-speech-cues-test.cc)

target_link_libraries(llm-speech-cues-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-speech-cues-test PRIVATE -Wall -Wextra)

set_target_properties(llm-speech-cues-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-speech-cues-test COMMAND llm-speech-cues-test)

add_executable(llm-speech-guard-test tests/unit/llm-speech-guard-test.cc)

target_link_libraries(llm-speech-guard-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-speech-guard-test PRIVATE -Wall -Wextra)

set_target_properties(llm-speech-guard-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-speech-guard-test COMMAND llm-speech-guard-test)

add_executable(llm-refusal-rules-test tests/unit/llm-refusal-rules-test.cc)

target_link_libraries(llm-refusal-rules-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-refusal-rules-test PRIVATE -Wall -Wextra)

set_target_properties(llm-refusal-rules-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-refusal-rules-test COMMAND llm-refusal-rules-test)

add_executable(llm-context-selector-test tests/unit/llm-context-selector-test.cc)

target_link_libraries(llm-context-selector-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-context-selector-test PRIVATE -Wall -Wextra)

set_target_properties(llm-context-selector-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-context-selector-test COMMAND llm-context-selector-test)

set_tests_properties(llm-context-selector-test PROPERTIES
    ENVIRONMENT "ARGUS_TEST_CONTEXT_PACK=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/eval/context-scenarios.jsonl:${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/eval/context-axis.jsonl")

add_executable(llm-bundle-loader-test tests/unit/llm-bundle-loader-test.cc)

target_link_libraries(llm-bundle-loader-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-bundle-loader-test PRIVATE -Wall -Wextra)

set_target_properties(llm-bundle-loader-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-bundle-loader-test COMMAND llm-bundle-loader-test)

add_executable(llm-bundle-check tools/bundle-check.cc)

target_link_libraries(llm-bundle-check PRIVATE argus::llm)

target_compile_options(llm-bundle-check PRIVATE -Wall -Wextra)

set_target_properties(llm-bundle-check PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-bundle-check-decide
    COMMAND llm-bundle-check decide ${CMAKE_CURRENT_SOURCE_DIR}/../../models/decide/bundle-laya-mvp --skip-missing)

add_test(NAME llm-bundle-check-extract
    COMMAND llm-bundle-check extract ${CMAKE_CURRENT_SOURCE_DIR}/../../models/extract/bundle-gliner-pilot --skip-missing)

set_tests_properties(llm-bundle-check-decide llm-bundle-check-extract PROPERTIES SKIP_RETURN_CODE 77)

add_executable(llm-bundle-golden-test tests/unit/llm-bundle-golden-test.cc)

target_link_libraries(llm-bundle-golden-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-bundle-golden-test PRIVATE -Wall -Wextra)

set_target_properties(llm-bundle-golden-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-bundle-golden-test COMMAND llm-bundle-golden-test)

add_executable(llm-turn-engines-test tests/unit/llm-turn-engines-test.cc)

target_compile_definitions(llm-turn-engines-test PRIVATE
    ARGUS_TEST_BUNDLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/bundles"
    ARGUS_TEST_LAYA_LABELS="${CMAKE_CURRENT_SOURCE_DIR}/tools/laya-labels.tsv"
    ARGUS_TEST_PRODUCTION_TRAFFIC="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/intent/eval-production.tsv")

target_link_libraries(llm-turn-engines-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-turn-engines-test PRIVATE -Wall -Wextra)

set_target_properties(llm-turn-engines-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-turn-engines-test COMMAND llm-turn-engines-test)

add_executable(llm-sp-tokenizer-test tests/unit/llm-sp-tokenizer-test.cc)

target_compile_definitions(llm-sp-tokenizer-test PRIVATE
    ARGUS_TEST_LAYA_SEQUENCES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/laya/sequence-parity.json"
    ARGUS_TEST_LAYA_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/../../models/decide/bundle-laya-mvp"
    ARGUS_TEST_NFC_PAIRS="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/laya/nfc-pairs.json")

target_link_libraries(llm-sp-tokenizer-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-sp-tokenizer-test PRIVATE -Wall -Wextra)

set_target_properties(llm-sp-tokenizer-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-sp-tokenizer-test COMMAND llm-sp-tokenizer-test)

add_executable(llm-laya-runtime-test tests/unit/llm-laya-runtime-test.cc)

target_compile_definitions(llm-laya-runtime-test PRIVATE
    ARGUS_TEST_LAYA_PILOT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/laya/parity-pilot.json"
    ARGUS_TEST_LAYA_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/../../models/decide/bundle-laya-mvp")

target_link_libraries(llm-laya-runtime-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-laya-runtime-test PRIVATE -Wall -Wextra)

set_target_properties(llm-laya-runtime-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-laya-runtime-test COMMAND llm-laya-runtime-test)

add_executable(llm-gliner-decoder-test tests/unit/llm-gliner-decoder-test.cc)

target_compile_definitions(llm-gliner-decoder-test PRIVATE
    ARGUS_TEST_GLINER_OVERLAP="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gliner/overlap.json"
    ARGUS_TEST_GLINER_RANDOM="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gliner/random-overlap.json")

target_link_libraries(llm-gliner-decoder-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-gliner-decoder-test PRIVATE -Wall -Wextra)

set_target_properties(llm-gliner-decoder-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-gliner-decoder-test COMMAND llm-gliner-decoder-test)

add_executable(llm-gliner-runtime-test tests/unit/llm-gliner-runtime-test.cc)

target_compile_definitions(llm-gliner-runtime-test PRIVATE
    ARGUS_TEST_GLINER_PARITY="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gliner/parity-pilot.json"
    ARGUS_TEST_GLINER_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/../../models/extract/bundle-gliner-pilot")

target_link_libraries(llm-gliner-runtime-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-gliner-runtime-test PRIVATE -Wall -Wextra)

set_target_properties(llm-gliner-runtime-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-gliner-runtime-test COMMAND llm-gliner-runtime-test)

add_executable(llm-e2e-turn-test tests/unit/llm-e2e-turn-test.cc)

target_compile_definitions(llm-e2e-turn-test PRIVATE
    ARGUS_TEST_LAYA_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/../../models/decide/bundle-laya-mvp"
    ARGUS_TEST_GLINER_BUNDLE="${CMAKE_CURRENT_SOURCE_DIR}/../../models/extract/bundle-gliner-pilot")

target_link_libraries(llm-e2e-turn-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-e2e-turn-test PRIVATE -Wall -Wextra)

set_target_properties(llm-e2e-turn-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-e2e-turn-test COMMAND llm-e2e-turn-test)

add_executable(llm-spoken-intent-test tests/unit/llm-spoken-intent-test.cc)

target_link_libraries(llm-spoken-intent-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-spoken-intent-test PRIVATE -Wall -Wextra)

set_target_properties(llm-spoken-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-spoken-intent-test COMMAND llm-spoken-intent-test)

add_executable(llm-time-arguments-test tests/unit/llm-time-arguments-test.cc)

target_link_libraries(llm-time-arguments-test PRIVATE argus::llm argus::lib::doctest-main)

target_compile_options(llm-time-arguments-test PRIVATE -Wall -Wextra)

set_target_properties(llm-time-arguments-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-time-arguments-test COMMAND llm-time-arguments-test)

add_executable(llm-pending-intent-test tests/unit/llm-pending-intent-test.cc)

target_link_libraries(llm-pending-intent-test PRIVATE argus::llm argus::pending-intent
    argus::lib::doctest-main)

target_compile_definitions(llm-pending-intent-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(llm-pending-intent-test PRIVATE -Wall -Wextra)

set_target_properties(llm-pending-intent-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-pending-intent-test COMMAND llm-pending-intent-test)

add_executable(llm-tool-runtime-test tests/unit/llm-tool-runtime-test.cc)

target_link_libraries(llm-tool-runtime-test PRIVATE argus::llm
    argus::lib::doctest-main)

target_compile_options(llm-tool-runtime-test PRIVATE -Wall -Wextra)

set_target_properties(llm-tool-runtime-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-tool-runtime-test COMMAND llm-tool-runtime-test)

add_executable(encounter-closed-consumer-test
    tests/unit/encounter-closed-consumer-test.cc)

target_link_libraries(encounter-closed-consumer-test PRIVATE
    argus::encounter-closed argus::memory argus::lib::doctest-main)

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
    argus::encounter-closed argus::memory argus::lib::doctest-main)

target_compile_definitions(encounter-closed-live-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(encounter-closed-live-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-closed-live-test PROPERTIES
    EXCLUDE_FROM_ALL FALSE)

argus_nats_scratch_test(NAME encounter-closed-live-test)

add_executable(memory-replica-test tests/unit/memory-replica-test.cc)

target_link_libraries(memory-replica-test PRIVATE argus::memory
    argus::lib::doctest-main)

target_compile_definitions(memory-replica-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(memory-replica-test PRIVATE -Wall -Wextra)

set_target_properties(memory-replica-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-replica-test COMMAND memory-replica-test)

add_executable(memory-backpressure-test tests/unit/memory-backpressure-test.cc)

target_link_libraries(memory-backpressure-test PRIVATE argus::memory
    argus::lib::doctest-main)

target_compile_definitions(memory-backpressure-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

target_compile_options(memory-backpressure-test PRIVATE -Wall -Wextra)

set_target_properties(memory-backpressure-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-backpressure-test COMMAND memory-backpressure-test)

add_executable(memory-reminder-test tests/unit/memory-reminder-test.cc)

target_link_libraries(memory-reminder-test PRIVATE argus::memory
    argus::contracts::auth argus::lib::doctest-main)

target_compile_definitions(memory-reminder-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

target_compile_options(memory-reminder-test PRIVATE -Wall -Wextra)

set_target_properties(memory-reminder-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-reminder-test COMMAND memory-reminder-test)

add_executable(notification-reminder-calls-test tests/unit/notification-reminder-calls-test.cc)

target_link_libraries(notification-reminder-calls-test PRIVATE argus::memory argus::lib::doctest-main)

target_compile_options(notification-reminder-calls-test PRIVATE -Wall -Wextra)

set_target_properties(notification-reminder-calls-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME notification-reminder-calls-test COMMAND notification-reminder-calls-test)

add_executable(reminder-readback-test tests/unit/reminder-readback-test.cc)

target_link_libraries(reminder-readback-test PRIVATE argus::memory argus::lib::doctest-main)

target_compile_options(reminder-readback-test PRIVATE -Wall -Wextra)

set_target_properties(reminder-readback-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME reminder-readback-test COMMAND reminder-readback-test)

add_executable(call-time-test tests/unit/call-time-test.cc)

target_link_libraries(call-time-test PRIVATE argus::memory argus::lib::doctest-main)

target_compile_options(call-time-test PRIVATE -Wall -Wextra)

set_target_properties(call-time-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME call-time-test COMMAND call-time-test)

add_executable(repeat-rule-test tests/unit/repeat-rule-test.cc)

target_link_libraries(repeat-rule-test PRIVATE argus::memory argus::lib::doctest-main)

target_compile_options(repeat-rule-test PRIVATE -Wall -Wextra)

set_target_properties(repeat-rule-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME repeat-rule-test COMMAND repeat-rule-test)

add_executable(encounter-inbox-test tests/unit/encounter-inbox-test.cc)

target_link_libraries(encounter-inbox-test PRIVATE argus::memory
    argus::lib::doctest-main)

target_compile_definitions(encounter-inbox-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(encounter-inbox-test PRIVATE -Wall -Wextra)

set_target_properties(encounter-inbox-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME encounter-inbox-test COMMAND encounter-inbox-test)

add_executable(memory-forget-test tests/unit/memory-forget-test.cc)

target_link_libraries(memory-forget-test PRIVATE argus::memory
    argus::lib::doctest-main)

target_compile_definitions(memory-forget-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")

target_compile_options(memory-forget-test PRIVATE -Wall -Wextra)

set_target_properties(memory-forget-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-forget-test COMMAND memory-forget-test)

add_executable(memory-vec-layout-test tests/unit/memory-vec-layout-test.cc)

target_link_libraries(memory-vec-layout-test PRIVATE argus::memory
    argus::contracts::auth argus::lib::doctest-main)

target_compile_definitions(memory-vec-layout-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")

target_compile_options(memory-vec-layout-test PRIVATE -Wall -Wextra)

set_target_properties(memory-vec-layout-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME memory-vec-layout-test COMMAND memory-vec-layout-test)

add_executable(extract-tier-split-test tests/unit/extract-tier-split-test.cc)

target_link_libraries(extract-tier-split-test PRIVATE argus::memory
    argus::lib::doctest-main)

target_compile_definitions(extract-tier-split-test PRIVATE
    ARGUS_TEST_MEMORY_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/memory")

target_compile_options(extract-tier-split-test PRIVATE -Wall -Wextra)

set_target_properties(extract-tier-split-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME extract-tier-split-test COMMAND extract-tier-split-test)

add_subdirectory(tests/eval)

add_executable(llm-components-test tests/unit/llm-components-test.cc)

target_link_libraries(llm-components-test PRIVATE argus::llm-settings argus::llm-config
    argus::contracts::settings-wire argus::lib::doctest-main)
argus_runtime_rpath(llm-components-test)

target_compile_options(llm-components-test PRIVATE -Wall -Wextra)

set_target_properties(llm-components-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME llm-components-test COMMAND llm-components-test)

add_executable(memory-scope-test tests/unit/memory-scope-test.cc)
target_link_libraries(memory-scope-test PRIVATE argus::llm-shared argus::lib::doctest-main)
target_compile_options(memory-scope-test PRIVATE -Wall -Wextra)
set_target_properties(memory-scope-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME memory-scope-test COMMAND memory-scope-test)

add_executable(llm-conversation-config-test tests/unit/llm-conversation-config-test.cc)
target_link_libraries(llm-conversation-config-test PRIVATE argus::llm-config argus::lib::doctest-main)
target_compile_options(llm-conversation-config-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-config-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-config-test COMMAND llm-conversation-config-test)

add_executable(llm-conversation-repository-test tests/unit/llm-conversation-repository-test.cc)
target_link_libraries(llm-conversation-repository-test PRIVATE argus::conversation argus::contracts::llm
    argus::lib::sqlite argus::lib::doctest-main)
target_compile_definitions(llm-conversation-repository-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(llm-conversation-repository-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-repository-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-repository-test COMMAND llm-conversation-repository-test)

add_executable(llm-conversation-switch-note-test tests/unit/llm-conversation-switch-note-test.cc)
target_link_libraries(llm-conversation-switch-note-test PRIVATE argus::conversation argus::contracts::llm argus::lib::doctest-main)
target_compile_options(llm-conversation-switch-note-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-switch-note-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-switch-note-test COMMAND llm-conversation-switch-note-test)

add_executable(memory-recall-scope-test tests/unit/memory-recall-scope-test.cc)
target_link_libraries(memory-recall-scope-test PRIVATE argus::memory argus::llm-shared argus::lib::doctest-main)
target_compile_definitions(memory-recall-scope-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(memory-recall-scope-test PRIVATE -Wall -Wextra)
set_target_properties(memory-recall-scope-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME memory-recall-scope-test COMMAND memory-recall-scope-test)

add_executable(memory-scope-classifier-test tests/unit/memory-scope-classifier-test.cc)
target_link_libraries(memory-scope-classifier-test PRIVATE argus::memory argus::llm-shared argus::lib::doctest-main)
target_compile_options(memory-scope-classifier-test PRIVATE -Wall -Wextra)
set_target_properties(memory-scope-classifier-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME memory-scope-classifier-test COMMAND memory-scope-classifier-test)

add_executable(llm-conversation-service-test tests/unit/llm-conversation-service-test.cc)
target_link_libraries(llm-conversation-service-test PRIVATE argus::conversation argus::contracts::llm argus::lib::sqlite argus::lib::doctest-main)
target_compile_definitions(llm-conversation-service-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(llm-conversation-service-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-service-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-service-test COMMAND llm-conversation-service-test)

add_executable(llm-conversation-retention-test tests/unit/llm-conversation-retention-test.cc)
target_link_libraries(llm-conversation-retention-test PRIVATE argus::llm-settings argus::conversation argus::contracts::llm argus::lib::sqlite argus::lib::doctest-main)
target_compile_definitions(llm-conversation-retention-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(llm-conversation-retention-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-retention-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-retention-test COMMAND llm-conversation-retention-test)

add_executable(memory-recall-vec-e2e-test tests/unit/memory-recall-vec-e2e-test.cc)
target_link_libraries(memory-recall-vec-e2e-test PRIVATE argus::memory argus::contracts::auth argus::lib::doctest-main)
target_compile_definitions(memory-recall-vec-e2e-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql"
    ARGUS_TEST_MEMORY_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/memory")
target_compile_options(memory-recall-vec-e2e-test PRIVATE -Wall -Wextra)
set_target_properties(memory-recall-vec-e2e-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME memory-recall-vec-e2e-test COMMAND memory-recall-vec-e2e-test)
set_tests_properties(memory-recall-vec-e2e-test PROPERTIES SKIP_RETURN_CODE 77 LABELS phase2)

add_executable(llm-conversation-context-test tests/unit/llm-conversation-context-test.cc)
target_link_libraries(llm-conversation-context-test PRIVATE argus::conversation argus::contracts::llm argus::lib::doctest-main)
target_compile_options(llm-conversation-context-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-context-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-context-test COMMAND llm-conversation-context-test)

add_executable(llm-conversation-exchange-test tests/unit/llm-conversation-exchange-test.cc)
target_link_libraries(llm-conversation-exchange-test PRIVATE argus::conversation argus::contracts::llm argus::lib::sqlite argus::lib::doctest-main)
target_compile_definitions(llm-conversation-exchange-test PRIVATE
    ARGUS_TEST_MEMORY_SCHEMA="${CMAKE_CURRENT_SOURCE_DIR}/database/schema.sql")
target_compile_options(llm-conversation-exchange-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-exchange-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-exchange-test COMMAND llm-conversation-exchange-test)

add_executable(llm-conversation-purge-test tests/unit/llm-conversation-purge-test.cc)
target_link_libraries(llm-conversation-purge-test PRIVATE argus::conversation argus::lib::doctest-main)
target_compile_options(llm-conversation-purge-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-purge-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-purge-test COMMAND llm-conversation-purge-test)

add_executable(llm-controller-conversation-test tests/unit/llm-controller-conversation-test.cc)
target_link_libraries(llm-controller-conversation-test PRIVATE argus::llm argus::conversation argus::contracts::llm argus::lib::doctest-main)
target_compile_options(llm-controller-conversation-test PRIVATE -Wall -Wextra)
set_target_properties(llm-controller-conversation-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-controller-conversation-test COMMAND llm-controller-conversation-test)

add_executable(llm-chat-conversation-http-test tests/unit/llm-chat-conversation-http-test.cc)
target_link_libraries(llm-chat-conversation-http-test PRIVATE argus::llm argus::clients::llm argus::contracts::llm argus::lib::doctest-main)
target_compile_options(llm-chat-conversation-http-test PRIVATE -Wall -Wextra)
set_target_properties(llm-chat-conversation-http-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-chat-conversation-http-test COMMAND llm-chat-conversation-http-test)

add_executable(llm-conversation-transcript-test tests/unit/llm-conversation-transcript-test.cc)
target_link_libraries(llm-conversation-transcript-test PRIVATE argus::conversation argus::lib::doctest-main)
target_compile_options(llm-conversation-transcript-test PRIVATE -Wall -Wextra)
set_target_properties(llm-conversation-transcript-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-conversation-transcript-test COMMAND llm-conversation-transcript-test)

add_executable(llm-messenger-courier-test tests/unit/llm-messenger-courier-test.cc)
target_link_libraries(llm-messenger-courier-test PRIVATE argus::messenger argus::lib::doctest-main)
target_compile_options(llm-messenger-courier-test PRIVATE -Wall -Wextra)
set_target_properties(llm-messenger-courier-test PROPERTIES EXCLUDE_FROM_ALL FALSE)
add_test(NAME llm-messenger-courier-test COMMAND llm-messenger-courier-test)

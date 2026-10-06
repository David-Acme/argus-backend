add_library(llm-eval-core STATIC eval-case.cc eval-report.cc eval-score.cc)

target_include_directories(llm-eval-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(llm-eval-core PUBLIC Drogon::Drogon)

target_compile_options(llm-eval-core PRIVATE -Wall -Wextra)

add_executable(fast-tier-eval fast-tier-eval.cc)

target_link_libraries(fast-tier-eval PRIVATE llm-eval-core argus::llm)

target_compile_options(fast-tier-eval PRIVATE -Wall -Wextra)

set_target_properties(fast-tier-eval PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME fast-tier-eval
    COMMAND fast-tier-eval
        --cases ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/cases.jsonl
        --gates ${CMAKE_CURRENT_SOURCE_DIR}/gates.json
        --model ${CMAKE_CURRENT_SOURCE_DIR}/../../../../models/intent/intent.bin
        --real ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/intent/eval-production.tsv
        --report ${CMAKE_CURRENT_BINARY_DIR}/fast-tier-report.json)

set_tests_properties(fast-tier-eval PROPERTIES SKIP_RETURN_CODE 77 LABELS eval)

add_executable(eval-corpus-test eval-corpus-test.cc)

target_compile_definitions(eval-corpus-test PRIVATE
    ARGUS_EVAL_CASES="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/cases.jsonl")

target_link_libraries(eval-corpus-test PRIVATE llm-eval-core doctest::doctest)

target_compile_options(eval-corpus-test PRIVATE -Wall -Wextra)

set_target_properties(eval-corpus-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME eval-corpus-test COMMAND eval-corpus-test)

add_executable(eval-score-test eval-score-test.cc)

target_link_libraries(eval-score-test PRIVATE llm-eval-core doctest::doctest)

target_compile_options(eval-score-test PRIVATE -Wall -Wextra)

set_target_properties(eval-score-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME eval-score-test COMMAND eval-score-test)

add_executable(llm-tier-eval llm-tier-eval.cc eval-tools.cc)

target_link_libraries(llm-tier-eval PRIVATE llm-eval-core argus::llm)

target_compile_options(llm-tier-eval PRIVATE -Wall -Wextra)

set_target_properties(llm-tier-eval PROPERTIES EXCLUDE_FROM_ALL FALSE)

set(LLM_EVAL_ARGS
    --cases ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/cases.jsonl
    --gates ${CMAKE_CURRENT_SOURCE_DIR}/gates.json
    --llm-model ${CMAKE_CURRENT_SOURCE_DIR}/../../../../models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf
    --intent-model ${CMAKE_CURRENT_SOURCE_DIR}/../../../../models/intent/intent.bin
    --catalog ${CMAKE_CURRENT_SOURCE_DIR}/../../../settings/modules.json)

add_test(NAME llm-tier-smoke
    COMMAND llm-tier-eval ${LLM_EVAL_ARGS} --smoke
        --report ${CMAKE_CURRENT_BINARY_DIR}/llm-tier-smoke-report.json)

add_test(NAME llm-tier-eval
    COMMAND llm-tier-eval ${LLM_EVAL_ARGS}
        --report ${CMAKE_CURRENT_BINARY_DIR}/llm-tier-report.json)

set_tests_properties(llm-tier-smoke PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 1200)
set_tests_properties(llm-tier-eval PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 7200)

add_library(llm-eval-core STATIC eval-case.cc eval-report.cc)

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

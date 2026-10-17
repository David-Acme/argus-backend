add_library(llm-eval-core STATIC eval-case.cc eval-report.cc eval-score.cc)

target_include_directories(llm-eval-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(llm-eval-core PUBLIC Drogon::Drogon)

target_compile_options(llm-eval-core PRIVATE -Wall -Wextra)

add_executable(production-decider production-decider.cc eval-tools.cc)

target_link_libraries(production-decider PRIVATE llm-eval-core argus::llm)

target_compile_options(production-decider PRIVATE -Wall -Wextra)

set_target_properties(production-decider PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_executable(call-time-cli call-time-cli.cc)

target_link_libraries(call-time-cli PRIVATE argus::memory)

target_compile_options(call-time-cli PRIVATE -Wall -Wextra)

set_target_properties(call-time-cli PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_executable(eval-corpus-test eval-corpus-test.cc)

target_compile_definitions(eval-corpus-test PRIVATE
    ARGUS_EVAL_CASES="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/cases.jsonl")

target_link_libraries(eval-corpus-test PRIVATE llm-eval-core argus::lib::text argus::lib::doctest-main)

target_compile_options(eval-corpus-test PRIVATE -Wall -Wextra)

set_target_properties(eval-corpus-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME eval-corpus-test COMMAND eval-corpus-test)

foreach(sealed_key sealed sealed2 sealed3)
    add_test(NAME eval-${sealed_key}-hash
        COMMAND ${CMAKE_COMMAND}
            -DGATES=${CMAKE_CURRENT_SOURCE_DIR}/gates.json
            -DKEY=${sealed_key}
            -DDIR=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check-sealed.cmake)
endforeach()

find_program(ARGUS_PYTHON3 python3)

if(ARGUS_PYTHON3)
    add_test(NAME decider-eval-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/decider-eval-test.py)
    set_tests_properties(decider-eval-test PROPERTIES LABELS eval)

    add_test(NAME tool-visibility-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/tool-visibility-test.py)
    set_tests_properties(tool-visibility-test PROPERTIES LABELS eval)

    add_test(NAME round-report-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/round-report-test.py)
    set_tests_properties(round-report-test PROPERTIES LABELS eval)

    add_test(NAME run-round-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/run-round-test.py)
    set_tests_properties(run-round-test PROPERTIES LABELS eval)

    add_test(NAME conversation-eval-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/conversation-eval-test.py)
    set_tests_properties(conversation-eval-test PROPERTIES LABELS eval)

    add_test(NAME calibration-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/calibration-test.py)
    set_tests_properties(calibration-test PROPERTIES LABELS eval)

    add_test(NAME perf-eval-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/perf-eval-test.py)
    set_tests_properties(perf-eval-test PROPERTIES LABELS eval)

    add_test(NAME slot-eval-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/slot-eval-test.py)
    set_tests_properties(slot-eval-test PROPERTIES LABELS eval)

    add_test(NAME call-time-cli-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/call-time-cli-test.py)
    set_tests_properties(call-time-cli-test PROPERTIES LABELS eval
        ENVIRONMENT CALL_TIME_CLI=$<TARGET_FILE:call-time-cli>)

    add_test(NAME call-faithfulness-pin-eval-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/call-faithfulness-pin-eval-test.py)
    set_tests_properties(call-faithfulness-pin-eval-test PROPERTIES LABELS eval
        ENVIRONMENT "CALL_FAITHFULNESS_EVAL=$<TARGET_FILE:call-faithfulness-eval>;CALL_FAITHFULNESS_GATES=${CMAKE_CURRENT_SOURCE_DIR}/gates.json;CALL_FAITHFULNESS_SMOKE_GATES=${CMAKE_CURRENT_SOURCE_DIR}/gates-smoke.json;CALL_FAITHFULNESS_MISSING_MODEL=${CMAKE_CURRENT_BINARY_DIR}/no-such-llm.gguf")

    add_test(NAME laya-parity-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/laya-parity-test.py)
    set_tests_properties(laya-parity-test PROPERTIES LABELS eval)

    add_test(NAME speech-ttft-eval
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/speech-ttft-eval.py
            --url http://127.0.0.1:1/llm/v1/chat-stream --requests 1 --warmup 0)
    set_tests_properties(speech-ttft-eval PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm")

    add_test(NAME speech-ttft-guard-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/speech-ttft-guard-test.py)
    set_tests_properties(speech-ttft-guard-test PROPERTIES LABELS eval)

    add_test(NAME speech-examples-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/speech-examples-test.py)
    set_tests_properties(speech-examples-test PROPERTIES LABELS eval)

    add_test(NAME peruvian-markers-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/peruvian-markers-test.py)
    set_tests_properties(peruvian-markers-test PROPERTIES LABELS eval)
endif()

add_executable(eval-score-test eval-score-test.cc)

target_link_libraries(eval-score-test PRIVATE llm-eval-core argus::lib::text argus::lib::doctest-main)

target_compile_options(eval-score-test PRIVATE -Wall -Wextra)

set_target_properties(eval-score-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME eval-score-test COMMAND eval-score-test)

add_executable(eval-tools-test eval-tools-test.cc eval-tools.cc)

target_link_libraries(eval-tools-test PRIVATE llm-eval-core argus::llm argus::lib::doctest-main)

target_compile_options(eval-tools-test PRIVATE -Wall -Wextra)

set_target_properties(eval-tools-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME eval-tools-test COMMAND eval-tools-test)

add_executable(llm-tier-eval llm-tier-eval.cc eval-tools.cc)

target_link_libraries(llm-tier-eval PRIVATE llm-eval-core argus::llm)

target_compile_options(llm-tier-eval PRIVATE -Wall -Wextra)

set_target_properties(llm-tier-eval PROPERTIES EXCLUDE_FROM_ALL FALSE)

set(LLM_EVAL_ARGS
    --cases ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/cases.jsonl
    --gates ${CMAKE_CURRENT_SOURCE_DIR}/gates.json
    --llm-model ${CMAKE_CURRENT_SOURCE_DIR}/../../../../models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf
    --catalog ${CMAKE_CURRENT_SOURCE_DIR}/../../../settings/modules.json)

add_test(NAME llm-tier-smoke
    COMMAND llm-tier-eval ${LLM_EVAL_ARGS} --smoke --section llmSmoke
        --report ${CMAKE_CURRENT_BINARY_DIR}/llm-tier-smoke-report.json)

add_test(NAME llm-tier-eval
    COMMAND llm-tier-eval ${LLM_EVAL_ARGS}
        --report ${CMAKE_CURRENT_BINARY_DIR}/llm-tier-report.json)

set_tests_properties(llm-tier-smoke PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 1200)
set_tests_properties(llm-tier-eval PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 7200)

add_executable(call-faithfulness-eval call-faithfulness-eval.cc eval-tools.cc)

target_link_libraries(call-faithfulness-eval PRIVATE llm-eval-core argus::llm)

target_compile_options(call-faithfulness-eval PRIVATE -Wall -Wextra)

set_target_properties(call-faithfulness-eval PROPERTIES EXCLUDE_FROM_ALL FALSE)

set(CALL_FAITHFULNESS_ARGS
    --cases ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/call-faithfulness.jsonl
    --gates ${CMAKE_CURRENT_SOURCE_DIR}/gates.json
    --llm-model ${CMAKE_CURRENT_SOURCE_DIR}/../../../../models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf
    --prompt-es ${CMAKE_CURRENT_SOURCE_DIR}/../../../voice/tests/fixtures/call-prompt-es.txt
    --prompt-en ${CMAKE_CURRENT_SOURCE_DIR}/../../../voice/tests/fixtures/call-prompt-en.txt
    --regionalism-markers ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/peruvian-markers.txt
    --temperature 0.3 --seed 42)

set(CALL_FAITHFULNESS_SMOKE_ARGS
    --cases ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/call-faithfulness.jsonl
    --gates ${CMAKE_CURRENT_SOURCE_DIR}/gates-smoke.json
    --llm-model ${CMAKE_CURRENT_SOURCE_DIR}/../../../../models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf
    --prompt-es ${CMAKE_CURRENT_SOURCE_DIR}/../../../voice/tests/fixtures/call-prompt-es.txt
    --prompt-en ${CMAKE_CURRENT_SOURCE_DIR}/../../../voice/tests/fixtures/call-prompt-en.txt
    --regionalism-markers ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/peruvian-markers.txt
    --temperature 0.3 --seed 42)

add_test(NAME call-faithfulness-smoke
    COMMAND call-faithfulness-eval ${CALL_FAITHFULNESS_SMOKE_ARGS} --limit 3
        --report ${CMAKE_CURRENT_BINARY_DIR}/call-faithfulness-smoke-report.json)

add_test(NAME call-faithfulness-eval
    COMMAND call-faithfulness-eval ${CALL_FAITHFULNESS_ARGS}
        --report ${CMAKE_CURRENT_BINARY_DIR}/call-faithfulness-report.json)

set_tests_properties(call-faithfulness-smoke PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 1800)
set_tests_properties(call-faithfulness-eval PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 7200)

add_test(NAME call-faithfulness-render-acts
    COMMAND call-faithfulness-eval ${CALL_FAITHFULNESS_ARGS} --render-acts
        --reject-sample ${CMAKE_CURRENT_BINARY_DIR}/call-faithfulness-render-acts-rejects.jsonl
        --report ${CMAKE_CURRENT_BINARY_DIR}/call-faithfulness-render-acts-report.json)

set_tests_properties(call-faithfulness-render-acts PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 3600)

add_library(llm-context-eval STATIC context-scenarios.cc)

target_include_directories(llm-context-eval PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(llm-context-eval PUBLIC Drogon::Drogon argus::lib::text)

target_compile_options(llm-context-eval PRIVATE -Wall -Wextra)

add_executable(context-scenarios-eval context-scenarios-eval.cc eval-tools.cc)

target_link_libraries(context-scenarios-eval PRIVATE llm-eval-core llm-context-eval argus::duress argus::llm)

target_compile_options(context-scenarios-eval PRIVATE -Wall -Wextra)

set_target_properties(context-scenarios-eval PROPERTIES EXCLUDE_FROM_ALL FALSE)

set(CONTEXT_SCENARIOS_ARGS
    --cases ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/context-scenarios.jsonl
    --cases ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/context-axis.jsonl
    --gates ${CMAKE_CURRENT_SOURCE_DIR}/gates.json
    --llm-model ${CMAKE_CURRENT_SOURCE_DIR}/../../../../models/llm/LFM2.5-1.2B-Instruct-QAD-Q4_0.gguf
    --prompt-es ${CMAKE_CURRENT_SOURCE_DIR}/../../../voice/tests/fixtures/call-prompt-es.txt
    --prompt-en ${CMAKE_CURRENT_SOURCE_DIR}/../../../voice/tests/fixtures/call-prompt-en.txt
    --regionalism-markers ${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/peruvian-markers.txt
    --temperature 0.3 --seed 42)

add_test(NAME context-scenarios-smoke
    COMMAND context-scenarios-eval ${CONTEXT_SCENARIOS_ARGS}
        --gates ${CMAKE_CURRENT_SOURCE_DIR}/gates-smoke.json
        --scenario panic --scenario errand --limit 2
        --report ${CMAKE_CURRENT_BINARY_DIR}/context-scenarios-smoke-report.json)

add_test(NAME context-scenarios-eval
    COMMAND context-scenarios-eval ${CONTEXT_SCENARIOS_ARGS}
        --report ${CMAKE_CURRENT_BINARY_DIR}/context-scenarios-report.json
        --table ${CMAKE_CURRENT_BINARY_DIR}/context-scenarios-table.json)

set_tests_properties(context-scenarios-smoke PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 1800)
set_tests_properties(context-scenarios-eval PROPERTIES SKIP_RETURN_CODE 77 LABELS "eval;eval-llm" TIMEOUT 14400)

add_executable(context-checks-test context-checks-test.cc)

target_link_libraries(context-checks-test PRIVATE llm-eval-core llm-context-eval argus::lib::text argus::lib::doctest-main)

target_compile_options(context-checks-test PRIVATE -Wall -Wextra)

target_compile_definitions(context-checks-test PRIVATE
    ARGUS_CONTEXT_SCENARIOS="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/context-scenarios.jsonl"
    ARGUS_CONTEXT_AXIS="${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/context-axis.jsonl"
    ARGUS_CONTEXT_GATES="${CMAKE_CURRENT_SOURCE_DIR}/gates.json"
    ARGUS_CONTEXT_SMOKE_GATES="${CMAKE_CURRENT_SOURCE_DIR}/gates-smoke.json")

set_target_properties(context-checks-test PROPERTIES EXCLUDE_FROM_ALL FALSE)

add_test(NAME context-checks-test COMMAND context-checks-test)

if(ARGUS_PYTHON3)
    add_test(NAME context-scenarios-pin-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/context-scenarios-pin-test.py)
    set_tests_properties(context-scenarios-pin-test PROPERTIES LABELS eval
        ENVIRONMENT "CONTEXT_SCENARIOS_EVAL=$<TARGET_FILE:context-scenarios-eval>;CONTEXT_SCENARIOS_GATES=${CMAKE_CURRENT_SOURCE_DIR}/gates.json;CONTEXT_SCENARIOS_SMOKE_GATES=${CMAKE_CURRENT_SOURCE_DIR}/gates-smoke.json;CONTEXT_SCENARIOS_MISSING_MODEL=${CMAKE_CURRENT_BINARY_DIR}/no-such-llm.gguf;CONTEXT_SCENARIOS_CASES=${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/context-scenarios.jsonl:${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/eval/context-axis.jsonl")

    add_test(NAME context-blind-test
        COMMAND ${ARGUS_PYTHON3} -I ${CMAKE_CURRENT_SOURCE_DIR}/context-blind-test.py)
    set_tests_properties(context-blind-test PROPERTIES LABELS eval)
endif()

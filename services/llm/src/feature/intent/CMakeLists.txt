set(INTENT_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../models/intent/intent.bin)
set(INTENT_MODEL_PIN ${CMAKE_CURRENT_SOURCE_DIR}/models/intent.bin.sha256)

if(EXISTS ${INTENT_MODEL} AND EXISTS ${INTENT_MODEL_PIN})
    file(SHA256 ${INTENT_MODEL} INTENT_MODEL_SHA256)
    file(READ ${INTENT_MODEL_PIN} INTENT_EXPECTED_SHA256)
    string(STRIP "${INTENT_EXPECTED_SHA256}" INTENT_EXPECTED_SHA256)
    if(NOT INTENT_MODEL_SHA256 STREQUAL INTENT_EXPECTED_SHA256)
        message(FATAL_ERROR "models/intent/intent.bin sha256 mismatch: expected "
            "${INTENT_EXPECTED_SHA256}, got ${INTENT_MODEL_SHA256}. "
            "Republish from the intent-training project.")
    endif()
endif()

argus_module(NAME intent
    SOURCES
        services/fasttext-classifier.cc
        services/intent-router.cc
    INCLUDES
        ../..
    DEPENDS
        argus::lib::text
        argus::lib::phrase
        fasttext)

target_compile_options(argus_intent PRIVATE -Wall -Wextra)

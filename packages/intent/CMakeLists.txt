cmake_minimum_required(VERSION 3.25)
project(argus-intent VERSION 1.0.0 LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(INTENT_MODEL ${CMAKE_CURRENT_SOURCE_DIR}/../../models/intent/intent.bin)
set(INTENT_MODEL_PIN ${CMAKE_CURRENT_SOURCE_DIR}/models/intent.bin.sha256)

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

if(NOT TARGET argus::lib::text)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/text
                     ${CMAKE_BINARY_DIR}/text)
endif()

if(NOT TARGET argus::lib::phrase)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../lib/phrase
                     ${CMAKE_BINARY_DIR}/phrase)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/argus-module.cmake)

argus_module(NAME intent
    SOURCES
        src/shared/services/intent/fasttext-classifier.cc
        src/shared/services/intent/intent-router.cc
    INCLUDES
        src
    DEPENDS
        argus::lib::text
        argus::lib::phrase
        fasttext)

target_compile_options(argus_intent PRIVATE -Wall -Wextra)

find_package(doctest REQUIRED)

foreach(TEST_NAME IN ITEMS intent-router-test intent-accuracy-test)
    add_executable(${TEST_NAME} tests/unit/${TEST_NAME}.cc)
    target_compile_definitions(${TEST_NAME} PRIVATE
        ARGUS_TEST_INTENT_MODELS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../models/intent"
        ARGUS_TEST_INTENT_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    target_link_libraries(${TEST_NAME} PRIVATE argus_intent doctest::doctest)
    target_compile_options(${TEST_NAME} PRIVATE -Wall -Wextra)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endforeach()
